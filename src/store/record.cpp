#include "store/record.h"

#include <cstring>

using namespace std;

namespace store {

void to_record(const Event& e, LogRecord& out) {
    //Whole-struct clear first. It zeroes the reserved bytes and any field the message
    //does not carry in one pass, which is what keeps two records built from the same
    //message byte identical - and a page over them checksummable.
    memset(&out, 0, sizeof(out));

    out.seq = e.seq;
    out.kind = e.kind;
    out.flags = e.flags;
    out.side = e.side;

    uint8_t present = 0;

    if(e.has_ts) {
        out.ts_ns = e.ts_ns;
        present |= HAS_TS;
    }

    if(e.has_symbol) {
        //Measured then copied, rather than strncpy'd. An 8 character symbol fills the
        //field exactly and must NOT carry a terminator into the byte after it, which is
        //the one thing strncpy warns about and the thing wanted here. The memset above
        //already NUL padded anything shorter.
        const size_t n = strnlen(e.symbol, sizeof(out.symbol));

        memcpy(out.symbol, e.symbol, n);
        present |= HAS_SYMBOL;
    }

    if(e.has_order_id) {
        out.order_id = e.order_id;
        present |= HAS_ORDER_ID;
    }

    if(e.has_size) {
        out.size = e.size;
        present |= HAS_SIZE;
    }

    if(e.has_price) {
        out.price = e.price;
        present |= HAS_PRICE;
    }

    if(e.has_trade_id) {
        out.trade_id = e.trade_id;
        present |= HAS_TRADE_ID;
    }

    out.present = present;
}


void to_event(const LogRecord& r, Event& out) {
    out.seq = r.seq;
    out.kind = r.kind;
    out.flags = r.flags;
    out.side = r.side;

    out.has_ts = (r.present & HAS_TS) != 0;
    out.has_symbol = (r.present & HAS_SYMBOL) != 0;
    out.has_order_id = (r.present & HAS_ORDER_ID) != 0;
    out.has_size = (r.present & HAS_SIZE) != 0;
    out.has_price = (r.present & HAS_PRICE) != 0;
    out.has_trade_id = (r.present & HAS_TRADE_ID) != 0;

    out.ts_ns = out.has_ts ? r.ts_ns : 0;
    out.order_id = out.has_order_id ? r.order_id : 0;
    out.size = out.has_size ? r.size : 0;
    out.price = out.has_price ? r.price : 0;
    out.trade_id = out.has_trade_id ? r.trade_id : 0;

    //Copy the 8 stored bytes and terminate in the 9th, which is the slot Event carries
    //for exactly this case. memcpy rather than strncpy because the source is not
    //guaranteed to hold a terminator.
    memcpy(out.symbol, r.symbol, sizeof(r.symbol));
    out.symbol[sizeof(r.symbol)] = 0;

    //Derived on decode and regenerable from kind and flags, so it is not stored. Cleared
    //rather than left stale, since callers reuse one Event across a scan.
    out.detail.clear();

    //The reader's buffer is long gone by the time a record is read back
    out.raw = 0;
    out.raw_len = 0;
}

} //namespace store
