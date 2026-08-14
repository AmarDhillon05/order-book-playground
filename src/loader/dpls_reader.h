//Sequential reader for a DPLS capture, one message per call, built on libpcap.
//
//libpcap handles the pcapng container and the Ethernet/IPv4/UDP framing. Everything
//from the IEX-TP header inward is ours. The constructor advances to the first packet
//actually carrying DPLS (IEX-TP protocol 0x8005) and stops there; read_event() walks
//forward from it and never looks back, which is what building a book needs.
//
//libpcap's API is strictly sequential and does not expose a file offset, so the start
//location is reported as a packet index and a timestamp rather than a byte position.

#ifndef DPLS_READER_H
#define DPLS_READER_H

#include <cstdint>
#include <string>
#include <vector>

struct pcap;
typedef struct pcap pcap_t;

//IEX-TP protocol id for DEEP+ / DPLS
const uint16_t DPLS_PROTOCOL = 0x8005;

//Prices are fixed point with four decimals, held as exact integers
const int64_t PRICE_SCALE = 10000;

//pcapng packet timestamps run 37 s ahead of the exchange timestamps carried inside the
//messages. That is TAI, not drift. Only capture timestamps need it; a message carries
//its own clock already on UTC.
const int64_t TAI_UTC_OFFSET_NS = 37000000000LL;

const size_t IEXTP_LEN = 40;


//One decoded message. Fields a given type does not carry are left flagged absent
//rather than zeroed, so "size 0" stays distinguishable from "no size".
struct Event {
    int64_t seq;
    char kind;
    uint8_t flags;

    int64_t ts_ns;                  //Exchange timestamp, ns since epoch
    char symbol[9];                 //NUL terminated, trailing spaces stripped
    char side;                      //'B', 'S', '?' or 0 when the type has no side

    int64_t order_id;
    uint32_t size;
    int64_t price;
    int64_t trade_id;

    bool has_ts;
    bool has_symbol;
    bool has_order_id;
    bool has_size;
    bool has_price;
    bool has_trade_id;

    std::string detail;             //Type specific text, empty when there is none

    const uint8_t* raw;             //Points into the reader's message buffer
    size_t raw_len;
};


//Stream integrity findings. Each is recorded rather than thrown: the job here is to
//detect damage and mark it, not to recover from it, so a caller decides what a damaged
//stream is worth. All three latch trusted() to false.

//The sequence skipped forward: datagrams were lost between these two packets.
struct Gap {
    int64_t expected;               //Sequence the next message should have carried
    int64_t got;                    //Sequence it actually carried
    int64_t missing;                //got - expected, the count of lost messages
    int64_t ts_ns;                  //Capture time of the packet that exposed it
    long long packet;
};

//The stream rewound. Not a gap: the sequence space itself started over, so nothing
//before this point belongs to the same stream as anything after it.
struct Restart {
    int64_t prev_seq;
    int64_t prev_offset;
    int64_t new_seq;
    int64_t new_offset;
    int64_t ts_ns;
    long long packet;
};

//A packet holding fewer messages than its header claimed, which happens when a capture
//truncates a datagram. Sequence numbers cannot see this: the next packet still lines up
//because expect is computed from the header's count, not from messages actually read.
struct ShortPayload {
    int64_t seq;
    int claimed;
    int found;
    int64_t ts_ns;
    long long packet;
};


class DplsReader {
public:
    explicit DplsReader(const std::string& path);
    ~DplsReader();

    DplsReader(const DplsReader&) = delete;
    DplsReader& operator=(const DplsReader&) = delete;

    //Next message. Returns false at end of file.
    bool read_event(Event& out);

    //The same message as one formatted log line. Empty string at end of file.
    std::string readline();

    //Where the DPLS stream was found. libpcap gives no byte offset, so this is the
    //index of the first DPLS packet and the capture time it arrived.
    long long start_packet() const { return start_packet_; }
    int64_t start_ts_ns() const { return start_ts_ns_; }

    uint32_t session() const { return session_; }
    uint32_t channel() const { return channel_; }
    std::string group() const { return group_; }
    uint16_t port() const { return port_; }
    int linktype() const { return linktype_; }

    long long packets_read() const { return packets_read_; }
    long long events_read() const { return events_read_; }

    //Latches false at the first gap, restart or short payload and stays there, so a book
    //fed from this reader can mark its state untrusted from that point on.
    bool trusted() const { return trusted_; }

    const std::vector<Gap>& gaps() const { return gaps_; }
    const std::vector<Restart>& restarts() const { return restarts_; }
    const std::vector<ShortPayload>& short_payloads() const { return short_payloads_; }

private:
    //Advance to the next DPLS packet that carries messages. False at end of file.
    bool advance();

    //Pull the IEX-TP header out of one captured packet. False if it is not IEX-TP.
    bool parse_packet(const uint8_t* pkt, uint32_t caplen, uint16_t& protocol);

    //Take up the packet parse_packet just read, running the integrity checks against it
    void begin_packet();

    //Close out the packet being read, checking it held as many messages as it claimed
    void finish_packet();

    pcap_t* handle_;
    std::string path_;

    int linktype_;
    long long start_packet_;
    int64_t start_ts_ns_;
    uint32_t session_;
    uint32_t channel_;
    std::string group_;
    uint16_t port_;

    //Cursor into the payload of the packet libpcap currently holds. Nothing is copied
    //out of it except one message body at a time.
    const uint8_t* payload_;
    uint32_t payload_left_;
    int64_t seq_;

    //The single message buffer. IEX messages top out well under this.
    uint8_t msg_[1024];

    long long packets_read_;
    long long events_read_;
    long long packet_index_;

    //Integrity checking state. expect_ is the sequence the next message must carry; -1
    //means there is no expectation yet, either at the start or just after a restart
    //reset it.
    bool trusted_;
    int64_t expect_;
    int64_t prev_offset_;
    std::vector<Gap> gaps_;
    std::vector<Restart> restarts_;
    std::vector<ShortPayload> short_payloads_;

    //Fields of the packet currently being read, kept for the header cross check
    int pkt_count_;
    int64_t pkt_offset_;
    int64_t pkt_ts_ns_;
    int64_t pkt_seq_;
    int claimed_this_packet_;       //-1 when not positioned inside a packet
    int msgs_this_packet_;
};


//Decode one message body into an Event. Exposed so a book can decode without a reader.
void decode_message(const uint8_t* m, size_t len, int64_t seq, Event& out);

//Format an Event the way loader/inspect_pcap.py prints it, so output can be diffed
//against the Python tooling directly.
std::string format_event(const Event& e);

//Human name for a message type character
const char* kind_name(char kind);

#endif
