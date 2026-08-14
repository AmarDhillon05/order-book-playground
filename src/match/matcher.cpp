#include "match/matcher.h"

using namespace std;

namespace match {

//Compare in one pass and report the most specific thing that is wrong. Order identity is
//checked before size because a different order consumed is a priority failure, while the
//same orders split differently is only a sizing one, and collapsing the two would hide
//which is happening.
Verdict grade(const vector<Fill>& got, const vector<Fill>& expected) {
    //Fewer fills than the feed reported means the walk ran dry. More is not possible:
    //the matcher stops at the taker's size, which came from the same fills.
    if(got.size() < expected.size()) {
        return BOOK_TOO_THIN;
    }

    if(got.size() != expected.size()) {
        return WRONG_ORDERS;
    }

    for(size_t i = 0; i < got.size(); i++) {
        if(got[i].order_id != expected[i].order_id) {
            return WRONG_ORDERS;
        }
    }

    for(size_t i = 0; i < got.size(); i++) {
        if(got[i].size != expected[i].size) {
            return WRONG_SIZES;
        }
    }

    return EXACT;
}


const char* verdict_name(Verdict v) {
    switch(v) {
        case EXACT: return "exact";
        case WRONG_ORDERS: return "wrong-orders";
        case WRONG_SIZES: return "wrong-sizes";
        case BOOK_TOO_THIN: return "book-too-thin";
        case SKIPPED_HIDDEN: return "skipped-hidden";
        case SKIPPED_UNKNOWN_SIDE: return "skipped-unknown-side";
    }

    return "?";
}

} //namespace match
