#include "books/naive_book.h"

using namespace std;

namespace books {

//Retention is on by default, since a book that cannot answer a historical query is the
//unusual case. replay_until and the matching harness are both that case: each already
//holds the stream it is feeding in, so a retained copy would only double the memory.
NaiveBook::NaiveBook() : retain_(true), orphans_(0) {}

NaiveBook::NaiveBook(Retention r) : retain_(r == RETAIN_LOG), orphans_(0) {}


//push_back the id onto the level's list, creating the level if this price is new, then
//keep --end() in the order so its own slot can later be erased without a search.
void NaiveBook::enqueue(SymbolBook& sb, Order& o) {
    if(o.side == 'B') {
        list<int64_t>& q = sb.bids[o.price];
        q.push_back(o.id);
        o.where = --q.end();

    } else {
        list<int64_t>& q = sb.asks[o.price];
        q.push_back(o.id);
        o.where = --q.end();
    }
}


//Erase the slot the order remembered, which costs nothing, then drop the level if that
//emptied it. The find() is for reaching the level to test it, not for locating the order.
void NaiveBook::dequeue(SymbolBook& sb, const Order& o) {
    if(o.side == 'B') {
        BidLevels::iterator lv = sb.bids.find(o.price);

        if(lv != sb.bids.end()) {
            lv->second.erase(o.where);

            if(lv->second.empty()) {
                sb.bids.erase(lv);
            }
        }

    } else {
        AskLevels::iterator lv = sb.asks.find(o.price);

        if(lv != sb.asks.end()) {
            lv->second.erase(o.where);

            if(lv->second.empty()) {
                sb.asks.erase(lv);
            }
        }
    }
}


//Build the order, evict anything already holding this id, then insert into the id index
//and enqueue the stored copy rather than the local, since enqueue writes `where` back.
void NaiveBook::add(const Event& e) {
    if(!e.has_order_id || !e.has_price || !e.has_size) {
        return;
    }

    //'?' means the flags byte did not name a side. Nothing can be booked from it.
    if(e.side != 'B' && e.side != 'S') {
        return;
    }

    Order o;
    o.id = e.order_id;
    o.price = e.price;
    o.size = e.size;
    o.side = e.side;
    o.symbol = e.symbol;

    //An add reusing a live id would leave the old one stranded in its queue.
    //
    //The stranded order is NOT necessarily on this instrument. Order ids are
    //symbol-partitioned, but a stream restart begins the id space again, so after one the
    //same id can reappear under a different symbol. Dequeuing from this order's book would
    //then either unlink nothing - leaving the old id in its real level's queue, where it
    //reads as a phantom level with zero size - or, if that book happens to hold a level at
    //the same price, erase a list iterator belonging to a different list entirely.
    //
    //Done before taking a reference into books_, since the eviction may need a different
    //instrument's book than the one being added to.
    unordered_map<int64_t, Order>::iterator old = orders_.find(o.id);

    if(old != orders_.end()) {
        unordered_map<string, SymbolBook>::iterator ob = books_.find(old->second.symbol);

        if(ob != books_.end()) {
            dequeue(ob->second, old->second);
        }

        orders_.erase(old);
    }

    SymbolBook& sb = books_[o.symbol];

    pair<unordered_map<int64_t, Order>::iterator, bool> ins =
        orders_.insert(make_pair(o.id, o));

    enqueue(sb, ins.first->second);
}


//Find the order by id, unlink it from its price level, then drop it from the index. An id
//that was never seen is counted as an orphan rather than treated as an error.
void NaiveBook::remove(int64_t order_id) {
    unordered_map<int64_t, Order>::iterator it = orders_.find(order_id);

    if(it == orders_.end()) {
        orphans_++;
        return;
    }

    unordered_map<string, SymbolBook>::iterator b = books_.find(it->second.symbol);

    if(b != books_.end()) {
        dequeue(b->second, it->second);
    }

    orders_.erase(it);
}


//The queue priority rule lives here: a size reduction may keep its place in line, a price
//change or a size increase may not. The feed states the intent in bit 0 of the flags byte
//rather than leaving it to be inferred, and the decoder has already turned that into
//e.detail; the bit is read directly so this does not depend on a string.
//
//Mechanically: keeping the slot is a write to o.size and nothing else, losing it is a
//dequeue followed by an enqueue at the new price. A new size of zero retires the order.
void NaiveBook::modify(const Event& e) {
    if(!e.has_order_id) {
        return;
    }

    unordered_map<int64_t, Order>::iterator it = orders_.find(e.order_id);

    if(it == orders_.end()) {
        orphans_++;
        return;
    }

    Order& o = it->second;

    unordered_map<string, SymbolBook>::iterator b = books_.find(o.symbol);

    if(b == books_.end()) {
        return;
    }

    SymbolBook& sb = b->second;

    int64_t new_price = e.has_price ? e.price : o.price;
    uint32_t new_size = e.has_size ? e.size : o.size;

    bool maintain = (e.flags & 1) != 0;
    bool shrinking = new_size <= o.size;

    //Keeping the slot is only legal when the exchange says so and the change is a pure
    //reduction at the same price. A price change or a size increase goes to the back.
    if(maintain && shrinking && new_price == o.price) {
        o.size = new_size;

        if(o.size == 0) {
            dequeue(sb, o);
            orders_.erase(it);
        }

        return;
    }

    dequeue(sb, o);

    if(new_size == 0) {
        orders_.erase(it);
        return;
    }

    o.price = new_price;
    o.size = new_size;
    enqueue(sb, o);
}


//An execution takes shares off a resting order. The price on the message is the TRADE
//price, which price improvement can move away from the order's booking price, so it is
//deliberately not read here.
//
//A fill at or past the resting size dequeues the order and drops it from the index;
//anything short of that is a decrement in place, which keeps its queue position.
void NaiveBook::execute(const Event& e) {
    if(!e.has_order_id) {
        return;
    }

    unordered_map<int64_t, Order>::iterator it = orders_.find(e.order_id);

    if(it == orders_.end()) {
        orphans_++;
        return;
    }

    Order& o = it->second;
    uint32_t filled = e.has_size ? e.size : o.size;

    if(filled >= o.size) {
        unordered_map<string, SymbolBook>::iterator b = books_.find(o.symbol);

        if(b != books_.end()) {
            dequeue(b->second, o);
        }

        orders_.erase(it);
        return;
    }

    //A partial fill is a reduction, so the order holds its place in the queue
    o.size -= filled;
}


//Wipe one instrument: a linear sweep of the id index for orders carrying this symbol,
//then erase the SymbolBook itself, which takes every level and queue with it.
void NaiveBook::clear_symbol(const string& symbol) {
    unordered_map<string, SymbolBook>::iterator b = books_.find(symbol);

    if(b == books_.end()) {
        return;
    }

    //The id index has to be swept too, or cancels for wiped orders read as orphans
    unordered_map<int64_t, Order>::iterator it = orders_.begin();

    while(it != orders_.end()) {
        if(it->second.symbol == symbol) {
            it = orders_.erase(it);
        } else {
            ++it;
        }
    }

    //The instrument is dropped entirely and reappears on its next message, so a retired
    //symbol costs nothing until it trades again
    books_.erase(b);
}


//The one ingest path: append to the log, then switch on the type character to whichever
//handler moves the book. Anything without a book effect falls through the switch.
void NaiveBook::apply(const Event& e) {
    //Retain first and unconditionally. The log is the whole stream, not the subset with
    //a book effect, because an event query is allowed to ask for the administrative
    //messages too.
    if(retain_) {
        log_.push_back(e);
    }

    switch(e.kind) {
        case 'a': add(e); break;
        case 'R': remove(e.order_id); break;
        case 'M': modify(e); break;
        case 'L': execute(e); break;

        //A trade report names a trade, not a resting order, so the book does not move
        case 'T': break;

        case 'C': clear_symbol(e.symbol); break;

        //Administrative and reference messages carry no book effect
        default: break;
    }
}


//begin() on each map is the inside price, since the comparators put it first. The size
//there is summed by walking that level's id list back through the order index.
TopOfBook NaiveBook::get_top_of_book(const string& symbol) const {
    TopOfBook t;
    t.bid_price = 0;
    t.bid_size = 0;
    t.ask_price = 0;
    t.ask_size = 0;
    t.has_bid = false;
    t.has_ask = false;

    unordered_map<string, SymbolBook>::const_iterator b = books_.find(symbol);

    if(b == books_.end()) {
        return t;
    }

    const SymbolBook& sb = b->second;

    if(!sb.bids.empty()) {
        BidLevels::const_iterator lv = sb.bids.begin();
        t.bid_price = lv->first;
        t.has_bid = true;

        for(list<int64_t>::const_iterator i = lv->second.begin();
            i != lv->second.end(); ++i) {
            unordered_map<int64_t, Order>::const_iterator o = orders_.find(*i);

            if(o != orders_.end()) {
                t.bid_size += o->second.size;
            }
        }
    }

    if(!sb.asks.empty()) {
        AskLevels::const_iterator lv = sb.asks.begin();
        t.ask_price = lv->first;
        t.has_ask = true;

        for(list<int64_t>::const_iterator i = lv->second.begin();
            i != lv->second.end(); ++i) {
            unordered_map<int64_t, Order>::const_iterator o = orders_.find(*i);

            if(o != orders_.end()) {
                t.ask_size += o->second.size;
            }
        }
    }

    return t;
}


//Walk outward from begin() on the requested side, stopping after n levels, aggregating
//each level's ids into one rung. The same per level summation get_top_of_book does once.
void NaiveBook::get_depth(const string& symbol, char side, size_t n,
                      vector<LevelView>& out) const {
    out.clear();

    unordered_map<string, SymbolBook>::const_iterator b = books_.find(symbol);

    if(b == books_.end()) {
        return;
    }

    const SymbolBook& sb = b->second;

    if(side == 'B') {
        BidLevels::const_iterator lv = sb.bids.begin();

        for(; lv != sb.bids.end() && out.size() < n; ++lv) {
            LevelView v;
            v.price = lv->first;
            v.size = 0;
            v.order_count = static_cast<uint32_t>(lv->second.size());

            for(list<int64_t>::const_iterator i = lv->second.begin();
                i != lv->second.end(); ++i) {
                unordered_map<int64_t, Order>::const_iterator o = orders_.find(*i);

                if(o != orders_.end()) {
                    v.size += o->second.size;
                }
            }

            out.push_back(v);
        }

    } else {
        AskLevels::const_iterator lv = sb.asks.begin();

        for(; lv != sb.asks.end() && out.size() < n; ++lv) {
            LevelView v;
            v.price = lv->first;
            v.size = 0;
            v.order_count = static_cast<uint32_t>(lv->second.size());

            for(list<int64_t>::const_iterator i = lv->second.begin();
                i != lv->second.end(); ++i) {
                unordered_map<int64_t, Order>::const_iterator o = orders_.find(*i);

                if(o != orders_.end()) {
                    v.size += o->second.size;
                }
            }

            out.push_back(v);
        }
    }
}


//One get_top_of_book per instrument in the symbol map, emitted in hash order rather than
//sorted, since nothing downstream depends on the ordering.
void NaiveBook::get_all_tops(vector<NamedTop>& out) const {
    out.clear();
    out.reserve(books_.size());

    for(unordered_map<string, SymbolBook>::const_iterator b = books_.begin();
        b != books_.end(); ++b) {
        out.push_back(make_pair(b->first, get_top_of_book(b->first)));
    }
}


//The walk itself, shared by both sides. Templated on the level map because bids and asks
//differ only in their comparator, and the copy-paste alternative is how two halves of one
//rule drift apart.
//
//The nesting IS price-time priority: the outer loop is the price half, the inner loop the
//time half. Walking them in that order visits resting orders in exactly the sequence an
//incoming order consumes them.
//
//It stops the moment the taker is filled, which is the payoff for matching inside the
//book. A sweep touches a few orders at the inside and never looks at the depth behind
//them; flattening the side for an outside matcher would have had to build all of it first.
template <class Levels>
static void walk_levels(const Levels& levels,
                    const unordered_map<int64_t, Order>& orders,
                    uint32_t size, vector<Fill>& out) {
    uint32_t left = size;

    for(typename Levels::const_iterator lv = levels.begin();
        lv != levels.end() && left > 0; ++lv) {

        for(list<int64_t>::const_iterator i = lv->second.begin();
            i != lv->second.end() && left > 0; ++i) {

            unordered_map<int64_t, Order>::const_iterator o = orders.find(*i);

            //A queued id with no order behind it should not happen, since both sides are
            //maintained together. Skipping rather than filling keeps a bookkeeping bug
            //from being reported as a phantom fill.
            if(o == orders.end()) {
                continue;
            }

            uint32_t take = (left < o->second.size) ? left : o->second.size;

            Fill f;
            f.order_id = o->second.id;
            f.size = take;
            f.price = lv->first;

            out.push_back(f);
            left -= take;
        }
    }
}


//A taker consumes the side opposite its own, so a buy eats the asks. Running out of book
//before the size is filled returns a short list rather than failing; how often that
//happens is a result worth counting, not an error worth throwing.
void NaiveBook::match(const string& symbol, char side, uint32_t size,
                  vector<Fill>& out) const {
    out.clear();

    unordered_map<string, SymbolBook>::const_iterator b = books_.find(symbol);

    if(b == books_.end()) {
        return;
    }

    if(side == 'B') {
        walk_levels(b->second.asks, orders_, size, out);
    } else {
        walk_levels(b->second.bids, orders_, size, out);
    }
}


//A straight lookup in the id index. Zero means the order is not resting, which at the head
//of a capture usually means it was added before the stream opened.
char NaiveBook::get_order_side(int64_t order_id) const {
    unordered_map<int64_t, Order>::const_iterator it = orders_.find(order_id);
    return (it == orders_.end()) ? 0 : it->second.side;
}


//Shared by every spread path: subtract the two prices, and leave it invalid with a zero
//width unless both sides were actually present.
static Spread spread_from(const TopOfBook& t) {
    Spread s;
    s.bid_price = t.bid_price;
    s.ask_price = t.ask_price;
    s.width = 0;
    s.valid = t.has_bid && t.has_ask;

    if(s.valid) {
        s.width = t.ask_price - t.bid_price;
    }

    return s;
}


//Read the top of book and subtract. This book keeps no spread of its own to read instead.
Spread NaiveBook::get_spread(const string& symbol) const {
    return spread_from(get_top_of_book(symbol));
}


//An absent filter field does not constrain. An event that cannot answer a field that was
//filtered on is dropped rather than kept, so a symbol query never returns the system
//messages that have no symbol at all.
static bool matches(const Event& e, const EventQuery& q) {
    if(q.symbol) {
        if(!e.has_symbol || *q.symbol != e.symbol) {
            return false;
        }
    }

    if(q.kinds && q.kinds->find(e.kind) == string::npos) {
        return false;
    }

    if(q.from_ts_ns || q.to_ts_ns) {
        if(!e.has_ts) {
            return false;
        }

        if(q.from_ts_ns && e.ts_ns < *q.from_ts_ns) {
            return false;
        }

        //Half open: the upper bound is not included
        if(q.to_ts_ns && e.ts_ns >= *q.to_ts_ns) {
            return false;
        }
    }

    return true;
}


//One linear pass over the retained log, copying out whatever the filter accepts. Order is
//preserved because the log is appended to and never sorted.
void NaiveBook::get_events(const EventQuery& q, vector<Event>& out) const {
    out.clear();

    for(size_t i = 0; i < log_.size(); i++) {
        if(matches(log_[i], q)) {
            out.push_back(log_[i]);
        }
    }
}


//Timestamps are not monotonic within a packet, so this cannot stop at the first event
//past ts_ns. The whole log is scanned and everything at or below the cutoff is applied,
//in the order it arrived.
void NaiveBook::replay_until(int64_t ts_ns, NaiveBook& into) const {
    for(size_t i = 0; i < log_.size(); i++) {
        const Event& e = log_[i];

        if(e.has_ts && e.ts_ns > ts_ns) {
            continue;
        }

        into.apply(e);
    }
}


//Replay the whole log into a throwaway non-retaining book, then read its present state.
//A full pass per point query, which is the ingest-cheap side of the tradeoff in the header.
TopOfBook NaiveBook::get_top_of_book_at(const string& symbol, int64_t ts_ns) const {
    NaiveBook scratch(DROP_LOG);
    replay_until(ts_ns, scratch);
    return scratch.get_top_of_book(symbol);
}


//The point in time top of book, subtracted. Pays the same full replay as the call it wraps.
Spread NaiveBook::get_spread_at(const string& symbol, int64_t ts_ns) const {
    return spread_from(get_top_of_book_at(symbol, ts_ns));
}


//Both series walk the log once, holding a book that is current as of each event, and
//emit only where the value actually moved. Events before the window still have to be
//applied, since the book at from_ts_ns depends on all of them; they just do not emit.
void NaiveBook::get_top_of_book_series(const string& symbol, int64_t from_ts_ns,
                                   int64_t to_ts_ns, vector<TopSample>& out) const {
    out.clear();

    NaiveBook scratch(DROP_LOG);

    //prev tracks the top across the whole replay, not just inside the window. Starting it
    //fresh at the window edge would make the first in-window event look like a change
    //even when it moved nothing, which is how an empty book emits a bogus leading sample.
    TopOfBook prev = scratch.get_top_of_book(symbol);

    for(size_t i = 0; i < log_.size(); i++) {
        const Event& e = log_[i];
        scratch.apply(e);

        //Only an event on this instrument can move its top of book
        if(!e.has_symbol || symbol != e.symbol) {
            continue;
        }

        TopOfBook now = scratch.get_top_of_book(symbol);

        bool changed = now.has_bid != prev.has_bid
                    || now.has_ask != prev.has_ask
                    || now.bid_price != prev.bid_price
                    || now.ask_price != prev.ask_price
                    || now.bid_size != prev.bid_size
                    || now.ask_size != prev.ask_size;

        prev = now;

        if(!changed) {
            continue;
        }

        if(e.has_ts && e.ts_ns >= from_ts_ns && e.ts_ns < to_ts_ns) {
            TopSample s;
            s.ts_ns = e.ts_ns;
            s.top = now;
            out.push_back(s);
        }
    }
}


//Built on the top of book series rather than on a second replay, then thinned again: only
//the samples where a price actually moved are kept.
void NaiveBook::get_spread_series(const string& symbol, int64_t from_ts_ns, int64_t to_ts_ns,
                              vector<SpreadSample>& out) const {
    out.clear();

    vector<TopSample> tops;
    get_top_of_book_series(symbol, from_ts_ns, to_ts_ns, tops);

    //A top of book change need not move the spread, since a size change on either side
    //leaves the prices alone. Collapse those out so the series is what it claims to be.
    Spread prev;
    prev.valid = false;
    prev.bid_price = 0;
    prev.ask_price = 0;
    prev.width = 0;
    bool have_prev = false;

    for(size_t i = 0; i < tops.size(); i++) {
        Spread s = spread_from(tops[i].top);

        //The first sample in the window always counts: top_of_book_series has already
        //dropped anything that did not move, so whatever survives to here is a change.
        bool changed = !have_prev
                    || s.valid != prev.valid
                    || s.bid_price != prev.bid_price
                    || s.ask_price != prev.ask_price;

        if(changed) {
            SpreadSample sample;
            sample.ts_ns = tops[i].ts_ns;
            sample.spread = s;
            out.push_back(sample);

            prev = s;
            have_prev = true;
        }
    }
}


//Walks every live order, which is exactly what get_checksum() avoids doing. Kept apart
//from it for that reason: this is an auditing call for the drift harness, not a digest.
long long NaiveBook::audit_resting_shares() const {
    long long total = 0;

    for(unordered_map<int64_t, Order>::const_iterator it = orders_.begin();
        it != orders_.end(); ++it) {
        total += it->second.size;
    }

    return total;
}


//Prices only, so this stays O(symbols) rather than walking every resting order
int64_t NaiveBook::get_checksum() const {
    int64_t h = static_cast<int64_t>(orders_.size());

    for(unordered_map<string, SymbolBook>::const_iterator b = books_.begin();
        b != books_.end(); ++b) {
        const SymbolBook& sb = b->second;

        if(!sb.bids.empty()) {
            h += sb.bids.begin()->first;
        }

        if(!sb.asks.empty()) {
            h -= sb.asks.begin()->first;
        }
    }

    return h;
}

} //namespace books
