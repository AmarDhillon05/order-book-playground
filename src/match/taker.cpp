#include "match/taker.h"

#include <algorithm>
#include <map>
#include <utility>

using namespace std;

namespace match {

//One matching instant. Symbol as well as timestamp, because unrelated instruments trade
//on the same nanosecond and are not the same event.
typedef pair<int64_t, string> Key;
typedef map<Key, vector<size_t> > Groups;


static bool by_first_index(const Taker& a, const Taker& b) {
    return a.first_index < b.first_index;
}


//Index the whole stream by instant first, then build one taker per instant, rather than
//scanning for runs. Grouping through a map sidesteps the non-monotonic timestamps
//entirely: an unrelated message landing mid sweep is simply an index that never joins
//this key, where a run scan would read it as the end of the group.
void extract_takers(const vector<Event>& events, vector<Taker>& out) {
    out.clear();

    Groups fills;                   //executions, by instant
    Groups prints;                  //trade reports, by instant

    for(size_t i = 0; i < events.size(); i++) {
        const Event& e = events[i];

        if(!e.has_ts || !e.has_symbol) {
            continue;
        }

        if(e.kind == 'L' && e.has_order_id) {
            fills[Key(e.ts_ns, e.symbol)].push_back(i);

        } else if(e.kind == 'T') {
            prints[Key(e.ts_ns, e.symbol)].push_back(i);
        }
    }

    out.reserve(fills.size());

    for(Groups::const_iterator g = fills.begin(); g != fills.end(); ++g) {
        const vector<size_t>& idx = g->second;

        Taker t;
        t.ts_ns = g->first.first;
        t.symbol = g->first.second;
        t.side = 0;                 //Resolved against the book during the replay
        t.size = 0;
        t.first_index = idx.front();

        //A print on the same instrument at the same nanosecond is the non-displayed leg
        //of this same matching pass, so shares the book cannot see are missing from the
        //total below. Flagged on the instant alone rather than on falling inside the
        //span, since a single fill group has no span to fall inside and would otherwise
        //never be caught. That over-flags where two unrelated trades share a nanosecond,
        //which is the safe direction: a flagged group is reported apart, not silently
        //counted as a clean comparison.
        t.hidden = (prints.find(g->first) != prints.end());

        for(size_t k = 0; k < idx.size(); k++) {
            const Event& e = events[idx[k]];

            Fill f;
            f.order_id = e.order_id;
            f.size = e.has_size ? e.size : 0;
            f.price = e.has_price ? e.price : 0;

            t.expected.push_back(f);
            t.size += f.size;
        }

        //Sweep the span for liquidity this instrument lost mid pass. Bounded by the
        //group's own extent, so a single fill group has nothing to collect.
        for(size_t j = idx.front() + 1; j < idx.back(); j++) {
            const Event& e = events[j];

            if((e.kind == 'R' || e.kind == 'M') && e.has_symbol && t.symbol == e.symbol) {
                t.presteps.push_back(j);
            }
        }

        out.push_back(t);
    }

    //The map ordered these by timestamp, which the stream is not. The replay fires each
    //check at a stream position, so they have to come back in stream order.
    sort(out.begin(), out.end(), by_first_index);
}

} //namespace match
