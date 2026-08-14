//Build a small capture with deliberate gaps in it, to exercise the FR-6 checks.
//
//The real HIST captures are clean: 42,492,551 packets, zero sequence gaps. Detection
//code that never fires is not verified, so this copies a slice of a capture and drops
//chosen packets on the way out, producing a file whose gaps are known in advance.
//
//  ./make_gap_fixture <in.pcap> <out.pcap> <packets> <drop_at>...
//
//drop_at values are indices into the DPLS packets that carry messages, counted from 1.
//Whatever is dropped is reported, so a test can assert the reader finds exactly it.

#include <pcap.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <set>

using namespace std;

//Just enough of a header read to index DPLS packets. This tool never decodes a message, so
//it does not link the reader.
static uint16_t le16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static int64_t le64(const uint8_t* p) { int64_t v; memcpy(&v, p, 8); return v; }

//Copy packets straight through to the output, skipping any whose DPLS index appears in
//drop, so the result is a byte identical capture with holes at known sequence numbers.
int main(int argc, char** argv) {
    if(argc < 5) {
        fprintf(stderr, "usage: %s <in.pcap> <out.pcap> <packets> <drop_at>...\n", argv[0]);
        return 2;
    }

    long long want = atoll(argv[3]);

    set<long long> drop;
    for(int i = 4; i < argc; i++) {
        drop.insert(atoll(argv[i]));
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* in = pcap_open_offline(argv[1], errbuf);
    if(!in) {
        fprintf(stderr, "open %s: %s\n", argv[1], errbuf);
        return 1;
    }

    //Written as classic pcap rather than pcapng. libpcap reads both, and only the
    //Ethernet framing and payload matter to anything downstream.
    pcap_t* dead = pcap_open_dead(DLT_EN10MB, 65535);
    pcap_dumper_t* out = pcap_dump_open(dead, argv[2]);
    if(!out) {
        fprintf(stderr, "dump open %s: %s\n", argv[2], pcap_geterr(dead));
        return 1;
    }

    struct pcap_pkthdr* hdr;
    const u_char* data;
    long long idx = 0, written = 0, dropped = 0;

    while(pcap_next_ex(in, &hdr, &data) == 1 && written < want) {
        //Only messages-carrying DPLS packets are indexed, so a drop is always a real
        //hole in the sequence rather than a lost heartbeat
        bool carries = false;
        int64_t seq = 0;
        int count = 0;

        if(hdr->caplen >= 42 && ((data[12] << 8) | data[13]) == 0x0800) {
            size_t ip = 14;
            size_t ihl = (data[ip] & 0x0F) * 4;

            if(data[ip + 9] == 17) {
                size_t tp = ip + ihl + 8;

                if(hdr->caplen >= tp + 40 && le16(data + tp + 2) == 0x8005) {
                    count = le16(data + tp + 14);
                    seq = le64(data + tp + 24);
                    carries = (count > 0);
                }
            }
        }

        if(carries) {
            idx++;

            if(drop.count(idx)) {
                printf("dropping DPLS packet %lld: seq %lld, %d message(s)\n",
                       idx, static_cast<long long>(seq), count);
                dropped++;
                continue;
            }
        }

        pcap_dump(reinterpret_cast<u_char*>(out), hdr, data);
        written++;
    }

    pcap_dump_close(out);
    pcap_close(dead);
    pcap_close(in);

    printf("wrote %lld packets to %s, dropped %lld\n", written, argv[2], dropped);
    return 0;
}
