//Walk a DPLS capture and print decoded messages, one per line.
//
//  ./dpls_dump <capture.pcap> [count]
//
//Output matches loader/inspect_pcap.py dump, so the two can be diffed against each
//other to check this reader against the Python one.

#include "dpls_reader.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace std;

//Open the capture, print up to `count` decoded lines, then drain the integrity findings the
//reader accumulated on the way and say whether the stream stayed trusted.
int main(int argc, char** argv) {
    if(argc < 2) {
        cerr << "usage: " << argv[0] << " <capture.pcap> [count]\n";
        return 2;
    }

    long long count = (argc > 2) ? atoll(argv[2]) : 20;
    bool quiet = (argc > 3 && string(argv[3]) == "--quiet");

    try {
        DplsReader r(argv[1]);

        if(!quiet) {
            fprintf(stderr, "linktype=%d  DPLS starts at packet %lld  "
                            "session 0x%08x  group %s:%u\n\n",
                    r.linktype(), r.start_packet(), r.session(),
                    r.group().c_str(), r.port());
        }

        Event e;
        long long n = 0;

        while(n < count && r.read_event(e)) {
            if(!quiet) {
                cout << format_event(e) << "\n";
            }

            n++;
        }

        fprintf(stderr, "\n%lld events from %lld packets\n",
                r.events_read(), r.packets_read());

        //Report what was detected. Book state is untrusted from the first of these.
        for(size_t i = 0; i < r.restarts().size(); i++) {
            const Restart& x = r.restarts()[i];
            fprintf(stderr, "RESTART  packet %lld: offset %lld -> %lld, seq -> %lld\n",
                    x.packet, (long long)x.prev_offset, (long long)x.new_offset,
                    (long long)x.new_seq);
        }

        for(size_t i = 0; i < r.gaps().size(); i++) {
            const Gap& x = r.gaps()[i];
            fprintf(stderr, "GAP      packet %lld: expected seq %lld, got %lld, "
                            "%lld message(s) missing\n",
                    x.packet, (long long)x.expected, (long long)x.got,
                    (long long)x.missing);
        }

        for(size_t i = 0; i < r.short_payloads().size(); i++) {
            const ShortPayload& x = r.short_payloads()[i];
            fprintf(stderr, "SHORT    packet %lld: seq %lld claimed %d messages, "
                            "found %d\n",
                    x.packet, (long long)x.seq, x.claimed, x.found);
        }

        fprintf(stderr, "book state: %s  (%zu gap(s), %zu restart(s), %zu short)\n",
                r.trusted() ? "TRUSTED" : "UNTRUSTED",
                r.gaps().size(), r.restarts().size(), r.short_payloads().size());

    } catch(const exception& err) {
        cerr << "error: " << err.what() << "\n";
        return 1;
    }

    return 0;
}
