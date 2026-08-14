//Decoding the capture into memory, kept apart from main so that test and fixture
//programs can pull the same stream in without linking the benchmark's entry point.

#include "bench.h"

#include <cstdio>

using namespace std;

//Drain DplsReader into a vector, stopping early when a limit is given, and warn at the end
//if the reader latched the capture untrusted.
vector<Event> load_events(const string& path, long long limit) {
    DplsReader r(path);

    vector<Event> out;

    if(limit > 0) {
        out.reserve(static_cast<size_t>(limit));
    }

    Event e;

    while((limit <= 0 || static_cast<long long>(out.size()) < limit) && r.read_event(e)) {
        //e.raw points into the reader's single message buffer, which the next call
        //overwrites. Keeping the pointer past that call would dangle, so the snapshot
        //carries decoded fields only.
        e.raw = 0;
        e.raw_len = 0;

        out.push_back(e);
    }

    if(!r.trusted()) {
        fprintf(stderr, "warning: stream is UNTRUSTED "
                        "(%zu gap(s), %zu restart(s), %zu short) - "
                        "book state after the first is not comparable to a clean run\n",
                r.gaps().size(), r.restarts().size(), r.short_payloads().size());
    }

    return out;
}
