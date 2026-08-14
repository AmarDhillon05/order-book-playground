#include "dpls_reader.h"

#include <pcap.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace std;

const size_t ETH_LEN = 14;
const size_t UDP_LEN = 8;

//Second byte of an add order carries the side, as the ASCII digits IEX uses elsewhere
const uint8_t SIDE_BUY = 0x38;                  //'8'
const uint8_t SIDE_SELL = 0x35;                 //'5'

//The capture is little endian on the wire. Loading through memcpy keeps this correct
//on a big endian host and avoids an unaligned read on any host.
static uint16_t le16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t le32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int64_t le64(const uint8_t* p) {
    int64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}


//Message type character to a printable name. Null for a type this decoder does not know,
//which the formatter turns into a "?x" label rather than hiding.
const char* kind_name(char kind) {
    switch(kind) {
        //Book events, the ones that move order state
        case 'a': return "ADD";
        case 'R': return "DELETE";
        case 'M': return "MODIFY";
        case 'L': return "EXECUTE";
        case 'T': return "TRADE";

        //DEEP only. The aggregated view: one message per price level, not per order.
        //The type char is the side, same '8'/'5' convention as an add order's flags.
        case '8': return "LEVEL";
        case '5': return "LEVEL";

        //Reference and status, mostly pre-open
        case 'S': return "SYSTEM";
        case 'D': return "DIRECTORY";
        case 'H': return "STATUS";
        case 'O': return "OPHALT";
        case 'P': return "SSR";
        case 'I': return "RETAIL";
        case 'E': return "SECEVENT";
        case 'B': return "BREAK";
        case 'C': return "CLEARBOOK";
    }

    return 0;
}

//Status types carry a single ASCII code in the flags byte rather than a bitfield
static const char* system_event(uint8_t f) {
    switch(f) {
        case 'O': return "start-of-messages";
        case 'S': return "start-of-system-hours";
        case 'R': return "start-of-regular-hours";
        case 'M': return "end-of-regular-hours";
        case 'E': return "end-of-system-hours";
        case 'C': return "end-of-messages";
    }

    return 0;
}

//The same one byte lookup for a security's trading state
static const char* trading_status(uint8_t f) {
    switch(f) {
        case 'H': return "halted";
        case 'O': return "order-acceptance";
        case 'P': return "pre-cross";
        case 'T': return "trading";
    }

    return 0;
}

//Retail Liquidity Indicator, one byte of state per symbol
static const char* retail_indicator(uint8_t f) {
    switch(f) {
        case 0x20: return "none";
        case 0x41: return "buy-interest";
        case 0x42: return "sell-interest";
        case 0x43: return "buy-and-sell-interest";
    }

    return 0;
}

//A flags byte holding an ASCII code prints as that character when it is printable
static string code_or_hex(uint8_t f) {
    if(f >= 32 && f < 127) {
        return string(1, static_cast<char>(f));
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02x", f);
    return string(buf);
}


//Clear the event, lift the fields nearly every type shares, then branch on the type
//character for the rest. Every field is at a fixed offset, so each branch is a few loads
//guarded by a length check; nothing inside a message body is length prefixed.
void decode_message(const uint8_t* m, size_t len, int64_t seq, Event& out) {
    //Reset field by field. Event holds a std::string, so it cannot be memset.
    out.seq = seq;
    out.kind = static_cast<char>(m[0]);
    out.flags = m[1];
    out.raw = m;
    out.raw_len = len;

    out.ts_ns = 0;
    out.symbol[0] = '\0';
    out.side = 0;
    out.order_id = 0;
    out.size = 0;
    out.price = 0;
    out.trade_id = 0;

    out.has_ts = false;
    out.has_symbol = false;
    out.has_order_id = false;
    out.has_size = false;
    out.has_price = false;
    out.has_trade_id = false;

    out.detail.clear();

    //Type, flags, timestamp and symbol are shared by nearly every message, so they come
    //out before anything branches on the type
    if(len >= 10) {
        out.ts_ns = le64(m + 2);
        out.has_ts = true;
    }

    if(len >= 18) {
        memcpy(out.symbol, m + 10, 8);
        out.symbol[8] = '\0';

        for(int i = 7; i >= 0 && out.symbol[i] == ' '; i--) {
            out.symbol[i] = '\0';
        }

        out.has_symbol = true;
    }

    char kind = out.kind;
    uint8_t flags = out.flags;

    //System event is the only message with no symbol at all
    if(kind == 'S') {
        const char* d = system_event(flags);
        out.detail = d ? string(d) : code_or_hex(flags);

    } else if(kind == 'H') {
        const char* d = trading_status(flags);
        out.detail = d ? string(d) : code_or_hex(flags);

        if(len >= 22) {
            string reason(reinterpret_cast<const char*>(m + 18), 4);

            size_t b = reason.find_first_not_of(' ');
            size_t e = reason.find_last_not_of(' ');
            reason = (b == string::npos) ? "" : reason.substr(b, e - b + 1);

            if(!reason.empty()) {
                out.detail += " reason=" + reason;
            }
        }

    } else if(kind == 'O') {
        out.detail = (flags == 'N') ? "not-halted" : (flags == 'O') ? "halted"
                                                                   : code_or_hex(flags);

    } else if(kind == 'P') {
        out.detail = flags ? "in-effect" : "not-in-effect";

    } else if(kind == 'I') {
        const char* d = retail_indicator(flags);
        out.detail = d ? string(d) : code_or_hex(flags);

    //Security directory carries a round lot and a reference price, not a book event
    } else if(kind == 'D' && len >= 31) {
        uint32_t lot = le32(m + 18);
        out.price = le64(m + 22);
        out.has_price = true;

        char buf[64];
        snprintf(buf, sizeof(buf), "roundlot=%u luld_tier=%u", lot, m[30]);
        out.detail = buf;

    //A DEEP price level update replaces the whole level. size 0 means it is gone.
    } else if((kind == '8' || kind == '5') && len >= 30) {
        out.side = (kind == '8') ? 'B' : 'S';
        out.size = le32(m + 18);
        out.has_size = true;
        out.price = le64(m + 22);
        out.has_price = true;

        if(out.size == 0) {
            out.detail = "level-removed";
        }

    //Trade reports name a trade rather than an order, so they carry no order id
    } else if(kind == 'T' && len >= 38) {
        out.size = le32(m + 18);
        out.has_size = true;
        out.price = le64(m + 22);
        out.has_price = true;
        out.trade_id = le64(m + 30);
        out.has_trade_id = true;

    } else if(len >= 26) {
        out.order_id = le64(m + 18);
        out.has_order_id = true;

        if(len >= 38) {
            out.size = le32(m + 26);
            out.has_size = true;
            out.price = le64(m + 30);
            out.has_price = true;
        }

        //An execution carries the trade it was part of. The price here is the TRADE
        //price, which can differ from the order's booking price through price
        //improvement, so it must not be used to move the book level.
        if(len >= 46) {
            out.trade_id = le64(m + 38);
            out.has_trade_id = true;
        }
    }

    if(kind == 'a') {
        out.side = (flags == SIDE_BUY) ? 'B' : (flags == SIDE_SELL) ? 'S' : '?';

    //Order Modify carries the queue priority rule in bit 0 of its flags byte. This is
    //the queue priority distinction: a size reduction can keep queue position, a price change
    //cannot.
    } else if(kind == 'M') {
        out.detail = (flags & 1) ? "maintain-priority" : "reset-priority";
    }
}


//Exchange time, printed on a fixed UTC-5 clock the way the Python tooling does.
//Message timestamps are nanoseconds; the sub-microsecond part is rounded to nearest
//rather than truncated so these lines diff clean against inspect_pcap.py, which gets
//the same rounding out of datetime.fromtimestamp.
static string clock_str(int64_t ts_ns) {
    time_t secs = static_cast<time_t>(ts_ns / 1000000000LL);
    int usec = static_cast<int>(((ts_ns % 1000000000LL) + 500) / 1000);

    //Rounding up out of the last microsecond of a second carries into the second
    if(usec >= 1000000) {
        usec -= 1000000;
        secs += 1;
    }

    time_t et = secs - 5 * 3600;
    struct tm tm;
    gmtime_r(&et, &tm);

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d",
             tm.tm_hour, tm.tm_min, tm.tm_sec, usec);
    return string(buf);
}

//Fixed point printed exactly, without ever going through a double
static string price_str(int64_t px) {
    int64_t whole = px / PRICE_SCALE;
    int64_t frac = px % PRICE_SCALE;

    const char* sign = "";
    if(frac < 0) {
        frac = -frac;
    }

    if(px < 0 && whole == 0) {
        sign = "-";
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%s%lld.%04lld", sign,
             static_cast<long long>(whole), static_cast<long long>(frac));
    return string(buf);
}


//Assemble the log line field by field, printing only what the message actually carried, so
//the columns after the symbol vary by type. A message with no timestamp cannot be laid out
//that way at all and falls back to a raw hex dump.
string format_event(const Event& e) {
    ostringstream out;

    const char* name = kind_name(e.kind);
    string label = name ? string(name) : ("?" + string(1, e.kind));

    if(!e.has_ts) {
        out << left << setw(9) << label << " flags=0x"
            << hex << setfill('0') << setw(2) << static_cast<int>(e.flags)
            << setfill(' ') << dec << " raw=";

        for(size_t i = 0; i < e.raw_len; i++) {
            out << hex << setfill('0') << setw(2) << static_cast<int>(e.raw[i]);
        }

        return out.str();
    }

    out << clock_str(e.ts_ns) << " " << left << setw(9) << label << " "
        << setw(8) << (e.has_symbol ? e.symbol : "") << right;

    if(e.side) {
        out << " " << e.side;
    }

    if(e.has_order_id) {
        out << " oid=" << e.order_id;
    }

    if(e.has_size) {
        out << " size=" << e.size;
    }

    if(e.has_price) {
        out << " px=" << price_str(e.price);
    }

    //Both are genuine Trade IDs. They never collide because L covers executions against
    //a displayed order and T covers non-displayed trading against non-displayed, which
    //are disjoint sets of trades, not two views of the same one.
    if(e.has_trade_id) {
        out << " trade=" << e.trade_id;
    }

    if(!e.detail.empty()) {
        out << " " << e.detail;
    }

    //Side already says what the flags byte meant on an add; elsewhere it still matters
    if(e.kind == 'T') {
        out << " cond=0x" << hex << setfill('0') << setw(2)
            << static_cast<int>(e.flags) << setfill(' ') << dec;

    } else if(e.kind == 'R' || e.kind == 'M' || e.kind == 'L' || e.kind == 'I') {
        out << " flags=0x" << hex << setfill('0') << setw(2)
            << static_cast<int>(e.flags) << setfill(' ') << dec;
    }

    return out.str();
}


//Open the capture, insist on Ethernet framing, then scan forward to the first packet
//carrying DPLS and stop on it with the payload cursor already set, so the first
//read_event() continues from there rather than starting the search over.
DplsReader::DplsReader(const string& path)
    : handle_(0), path_(path), linktype_(-1), start_packet_(-1), start_ts_ns_(0),
      session_(0), channel_(0), port_(0), payload_(0), payload_left_(0), seq_(0),
      packets_read_(0), events_read_(0), packet_index_(0),
      trusted_(true), expect_(-1), prev_offset_(-1),
      pkt_count_(0), pkt_offset_(0), pkt_ts_ns_(0), pkt_seq_(0),
      claimed_this_packet_(-1), msgs_this_packet_(0) {

    char errbuf[PCAP_ERRBUF_SIZE];

    //libpcap reads pcapng as well as classic pcap, so the .pcap extension these files
    //carry does not matter here
    handle_ = pcap_open_offline(path.c_str(), errbuf);
    if(!handle_) {
        throw runtime_error("pcap_open_offline: " + string(errbuf));
    }

    linktype_ = pcap_datalink(handle_);
    if(linktype_ != DLT_EN10MB) {
        pcap_close(handle_);
        handle_ = 0;
        throw runtime_error(path + ": expected Ethernet, got linktype "
                            + to_string(linktype_));
    }

    //Walk forward to the first packet actually carrying DPLS and stop on it. If it has
    //a payload the cursor is already set, so read_event() picks up from there.
    struct pcap_pkthdr* hdr;
    const u_char* data;
    int rc;

    while((rc = pcap_next_ex(handle_, &hdr, &data)) == 1) {
        packet_index_++;

        uint16_t protocol = 0;
        if(!parse_packet(data, hdr->caplen, protocol) || protocol != DPLS_PROTOCOL) {
            payload_left_ = 0;
            continue;
        }

        start_packet_ = packet_index_;
        start_ts_ns_ = static_cast<int64_t>(hdr->ts.tv_sec) * 1000000000LL
                     + static_cast<int64_t>(hdr->ts.tv_usec) * 1000LL
                     - TAI_UTC_OFFSET_NS;
        pkt_ts_ns_ = start_ts_ns_;

        packets_read_++;

        //Seeds prev_offset_ and expect_ from this first packet. Nothing can fire here,
        //since there is no previous packet to compare against.
        begin_packet();

        if(payload_left_ == 0) {
            finish_packet();
        }

        return;
    }

    pcap_close(handle_);
    handle_ = 0;
    throw runtime_error(path + ": no DPLS packets found");
}


//Only libpcap's handle is owned here. Everything else is a value member or points into
//memory libpcap holds, so there is nothing else to release.
DplsReader::~DplsReader() {
    if(handle_) {
        pcap_close(handle_);
    }
}


//Step through Ethernet, IPv4 and UDP by fixed offsets to reach the IEX-TP header, read the
//session, sequence and count fields out of it, and leave payload_ pointing at the messages
//behind it. Nothing is copied: the cursor is a view into libpcap's packet.
bool DplsReader::parse_packet(const uint8_t* pkt, uint32_t caplen, uint16_t& protocol) {
    payload_ = 0;
    payload_left_ = 0;

    //Ethernet II carrying IPv4 carrying UDP, nothing else on this feed
    if(caplen < 42 || be16(pkt + 12) != 0x0800) {
        return false;
    }

    size_t ip = ETH_LEN;
    size_t ihl = (pkt[ip] & 0x0F) * 4;

    if(pkt[ip + 9] != 17) {
        return false;
    }

    size_t udp = ip + ihl;
    size_t iextp = udp + UDP_LEN;

    if(caplen < iextp + IEXTP_LEN) {
        return false;
    }

    protocol = le16(pkt + iextp + 2);
    channel_ = le32(pkt + iextp + 4);
    session_ = le32(pkt + iextp + 8);

    uint16_t plen = le16(pkt + iextp + 12);

    //The two counters the integrity checks run on. stream_offset is a byte position in the
    //stream, first_seq the message index of that same first message.
    pkt_count_ = le16(pkt + iextp + 14);
    pkt_offset_ = le64(pkt + iextp + 16);
    seq_ = le64(pkt + iextp + 24);
    pkt_seq_ = seq_;

    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             pkt[ip + 16], pkt[ip + 17], pkt[ip + 18], pkt[ip + 19]);
    group_ = buf;
    port_ = be16(pkt + udp + 2);

    //The payload cannot run past what was actually captured
    uint32_t avail = caplen - static_cast<uint32_t>(iextp + IEXTP_LEN);
    payload_ = pkt + iextp + IEXTP_LEN;
    payload_left_ = (plen < avail) ? plen : avail;

    return true;
}


//The integrity checks, run once per DPLS packet as it is taken up.
//
//Order matters. The restart check has to clear expect_ before the gap check runs, or a
//rewind registers as a gap of minus fifty million messages. Heartbeats carry no
//messages and must not touch expect_ at all, or they manufacture gaps.
void DplsReader::begin_packet() {
    //A stream offset that goes backwards means the feed restarted from scratch, so
    //nothing before this point shares a sequence space with anything after it
    if(prev_offset_ >= 0 && pkt_offset_ < prev_offset_) {
        Restart r;
        r.prev_seq = expect_;
        r.prev_offset = prev_offset_;
        r.new_seq = pkt_seq_;
        r.new_offset = pkt_offset_;
        r.ts_ns = pkt_ts_ns_;
        r.packet = packet_index_;
        restarts_.push_back(r);

        expect_ = -1;
        trusted_ = false;
    }

    prev_offset_ = pkt_offset_;

    if(pkt_count_ > 0) {
        if(expect_ >= 0 && pkt_seq_ != expect_) {
            Gap g;
            g.expected = expect_;
            g.got = pkt_seq_;
            g.missing = pkt_seq_ - expect_;
            g.ts_ns = pkt_ts_ns_;
            g.packet = packet_index_;
            gaps_.push_back(g);

            trusted_ = false;
        }

        expect_ = pkt_seq_ + pkt_count_;
    }

    claimed_this_packet_ = pkt_count_;
    msgs_this_packet_ = 0;
}


//The check sequence numbers cannot make. A truncated capture drops messages off the end
//of a datagram without disturbing the sequence, because expect_ came from the header.
void DplsReader::finish_packet() {
    if(claimed_this_packet_ < 0) {
        return;
    }

    if(msgs_this_packet_ != claimed_this_packet_) {
        ShortPayload s;
        s.seq = pkt_seq_;
        s.claimed = claimed_this_packet_;
        s.found = msgs_this_packet_;
        s.ts_ns = pkt_ts_ns_;
        s.packet = packet_index_;
        short_payloads_.push_back(s);

        trusted_ = false;
    }

    claimed_this_packet_ = -1;
}


//Pull packets until one is DPLS and actually carries messages, running begin_packet() on
//every DPLS packet along the way so heartbeats still advance the integrity state.
bool DplsReader::advance() {
    struct pcap_pkthdr* hdr;
    const u_char* data;
    int rc;

    while((rc = pcap_next_ex(handle_, &hdr, &data)) == 1) {
        packet_index_++;

        uint16_t protocol = 0;
        if(!parse_packet(data, hdr->caplen, protocol) || protocol != DPLS_PROTOCOL) {
            payload_left_ = 0;
            continue;
        }

        packets_read_++;

        pkt_ts_ns_ = static_cast<int64_t>(hdr->ts.tv_sec) * 1000000000LL
                   + static_cast<int64_t>(hdr->ts.tv_usec) * 1000LL
                   - TAI_UTC_OFFSET_NS;

        //Every DPLS packet is checked, heartbeats included: a restart is visible on a
        //heartbeat too, and its stream offset still has to advance prev_offset_
        begin_packet();

        //Heartbeats carry no messages, so there is nothing to hand back
        if(payload_left_ == 0) {
            finish_packet();
            continue;
        }

        return true;
    }

    return false;
}


//Copy the next length prefixed message out of the packet in hand and decode it, looping to
//the next packet whenever this one runs out or its tail turns out to be padding.
bool DplsReader::read_event(Event& out) {
    while(true) {
        //Two bytes of length prefix, then exactly that many bytes of body. One message
        //is copied out; libpcap owns the packet it came from and we never keep it.
        if(payload_left_ >= 2) {
            uint16_t mlen = le16(payload_);

            //A zero length, an overrun, or an implausibly long message means the rest
            //of this packet is padding, so drop it and move on
            if(mlen && static_cast<uint32_t>(2 + mlen) <= payload_left_
                    && mlen <= sizeof(msg_)) {

                memcpy(msg_, payload_ + 2, mlen);
                payload_ += 2 + mlen;
                payload_left_ -= 2 + mlen;

                decode_message(msg_, mlen, seq_, out);
                seq_++;
                events_read_++;
                msgs_this_packet_++;
                return true;
            }
        }

        //Leaving this packet, so its message count can be reconciled with its header
        finish_packet();
        payload_left_ = 0;

        if(!advance()) {
            return false;
        }
    }
}


//read_event() plus format_event(), for callers that only ever wanted the text
string DplsReader::readline() {
    Event e;
    if(!read_event(e)) {
        return string();
    }

    return format_event(e);
}
