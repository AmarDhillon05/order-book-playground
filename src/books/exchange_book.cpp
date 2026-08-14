#include "books/exchange_book.h"

using namespace std;

namespace books {

//Retention is on by default, matching NaiveBook: a book that cannot answer a historical
//query is the unusual case. The drift harness is that case - it already holds the stream
//it is feeding in - and constructs with DROP_LOG.
ExchangeBook::ExchangeBook()
    : retain_(true), next_id_(-1),
      crossed_adds_(0), crossed_add_shares_(0),
      crossed_modifies_(0), crossed_modify_shares_(0),
      orphans_(0), ignored_executes_(0),
      submitted_orders_(0), submitted_shares_(0),
      traded_taker_shares_(0), traded_maker_shares_(0),
      cancelled_shares_(0), resting_shares_(0), fills_(0) {}

ExchangeBook::ExchangeBook(Retention r)
    : retain_(r == RETAIN_LOG), next_id_(-1),
      crossed_adds_(0), crossed_add_shares_(0),
      crossed_modifies_(0), crossed_modify_shares_(0),
      orphans_(0), ignored_executes_(0),
      submitted_orders_(0), submitted_shares_(0),
      traded_taker_shares_(0), traded_maker_shares_(0),
      cancelled_shares_(0), resting_shares_(0), fills_(0) {}


//Identical to NaiveBook's: push_back onto the level's list, creating the level if the
//price is new, then remember --end() so the slot can later be erased without a search.
void ExchangeBook::enqueue(SymbolBook& sb, Order& o) {
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


void ExchangeBook::dequeue(SymbolBook& sb, const Order& o) {
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


//The matching walk, and the only place in this project where resting orders are consumed
//for real rather than reported on.
//
//The nesting is price-time priority: the outer loop is the price half, the inner loop the
//time half, so orders are taken in exactly the sequence the rule dictates. It is the same
//shape as NaiveBook's read-only walk, with three differences that matter:
//
//  - resting sizes are decremented and emptied orders removed, here, as it goes
//  - a price limit can stop it short of the book running out
//  - it erases levels it empties, so the book is left in a state the next order can walk
//
//A partially filled order keeps its place, which is the front of its queue, because a
//reduction never costs priority. That is the same rule the modify path applies, arrived
//at from the other direction.
template <class Levels>
uint32_t ExchangeBook::consume(Levels& levels, char taker_side, uint32_t size,
                           bool bounded, int64_t limit, vector<Fill>& fills) {
    uint32_t left = size;

    typename Levels::iterator lv = levels.begin();

    while(lv != levels.end() && left > 0) {
        //Levels run outward from the inside, so the first level past the limit means every
        //level behind it is past it too and the walk is finished, not merely skipping one.
        if(bounded) {
            if(taker_side == 'B' ? (lv->first > limit) : (lv->first < limit)) {
                break;
            }
        }

        list<int64_t>& q = lv->second;
        list<int64_t>::iterator it = q.begin();

        while(it != q.end() && left > 0) {
            unordered_map<int64_t, Order>::iterator o = orders_.find(*it);

            //A queued id with no order behind it is a bookkeeping bug. Drop the slot
            //rather than fill against it, which would create shares out of nothing and
            //break conservation in a way that reads as a matching error.
            if(o == orders_.end()) {
                it = q.erase(it);
                continue;
            }

            uint32_t take = (left < o->second.size) ? left : o->second.size;

            Fill f;
            f.order_id = o->second.id;
            f.size = take;

            //The resting order's booking price, which is the price this trade happens at.
            //Nothing here models price improvement, so there is no separate trade price.
            f.price = lv->first;

            fills.push_back(f);

            left -= take;
            o->second.size -= take;
            traded_maker_shares_ += take;
            resting_shares_ -= take;

            if(o->second.size == 0) {
                orders_.erase(o);
                it = q.erase(it);

            } else {
                //Filled short, so it stays where it is at the front of the queue
                ++it;
            }
        }

        if(q.empty()) {
            lv = levels.erase(lv);
        } else {
            ++lv;
        }
    }

    return size - left;
}


//Match, then rest or cancel the remainder. The id is resolved by the caller so that
//submit() can record it in the log before anything mutates.
ExecReport ExchangeBook::place(const NewOrder& n, int64_t id) {
    ExecReport r;
    r.order_id = id;

    if(n.side != 'B' && n.side != 'S') {
        return r;
    }

    if(n.size == 0) {
        return r;
    }

    submitted_orders_++;
    submitted_shares_ += n.size;

    //Found rather than created: a market order against an instrument this book has never
    //seen must not leave an empty SymbolBook behind, which would inflate the symbol count
    //and the memory-per-book figure with instruments that never traded.
    SymbolBook* sb = 0;
    unordered_map<string, SymbolBook>::iterator b = books_.find(n.symbol);

    if(b != books_.end()) {
        sb = &b->second;
    }

    bool bounded = (n.type != MARKET);
    uint32_t filled = 0;

    if(sb) {
        filled = (n.side == 'B')
            ? consume(sb->asks, 'B', n.size, bounded, n.price, r.fills)
            : consume(sb->bids, 'S', n.size, bounded, n.price, r.fills);
    }

    r.filled = filled;
    traded_taker_shares_ += filled;
    fills_ += static_cast<long long>(r.fills.size());

    uint32_t left = n.size - filled;

    if(left == 0) {
        return r;
    }

    //Only a limit order rests. A market or IOC remainder is cancelled, which is the whole
    //difference between the types as far as this book is concerned.
    if(n.type != LIMIT) {
        r.cancelled = left;
        cancelled_shares_ += left;
        return r;
    }

    if(!sb) {
        sb = &books_[n.symbol];
    }

    Order o;
    o.id = id;
    o.price = n.price;
    o.size = left;
    o.side = n.side;
    o.symbol = n.symbol;

    pair<unordered_map<int64_t, Order>::iterator, bool> ins =
        orders_.insert(make_pair(id, o));

    if(!ins.second) {
        //The id is already live. Every caller evicts first, so reaching here is a bug in
        //this file; cancel the shares rather than book an order no queue points at, which
        //would leave the id index and the queues permanently disagreeing.
        r.cancelled = left;
        cancelled_shares_ += left;
        return r;
    }

    enqueue(*sb, ins.first->second);

    r.resting = left;
    resting_shares_ += left;

    return r;
}


//The public entry point. Resolves the id, records the request, then places it.
//
//The log entry carries the RESOLVED id rather than the caller's 0, so replaying the log
//reproduces the same ids the live book assigned and a historical query answers about the
//same orders. Without that, a replay would renumber every injected order.
ExecReport ExchangeBook::submit(const NewOrder& n) {
    if(n.side != 'B' && n.side != 'S') {
        return ExecReport();
    }

    if(n.size == 0) {
        return ExecReport();
    }

    int64_t id = (n.client_id != 0) ? n.client_id : next_id_--;

    if(retain_) {
        LogEntry le;
        le.is_order = true;
        le.order = n;
        le.order.client_id = id;
        log_.push_back(le);
    }

    return place(n, id);
}


bool ExchangeBook::cancel(int64_t order_id) {
    unordered_map<int64_t, Order>::iterator it = orders_.find(order_id);

    if(it == orders_.end()) {
        orphans_++;
        return false;
    }

    unordered_map<string, SymbolBook>::iterator b = books_.find(it->second.symbol);

    if(b != books_.end()) {
        dequeue(b->second, it->second);
    }

    resting_shares_ -= it->second.size;
    cancelled_shares_ += it->second.size;
    orders_.erase(it);

    return true;
}


//A feed add becomes a fresh limit order, which is the whole inversion: the mirror books
//place it, this one offers it up to be matched first.
//
//IEX published this order as RESTING, so at IEX it crossed nothing. If it fills anything
//here, this book was holding liquidity IEX's was not, and that is drift observed from the
//inside with no second book to compare against.
void ExchangeBook::on_add(const Event& e) {
    if(!e.has_order_id || !e.has_price || !e.has_size) {
        return;
    }

    //'?' means the flags byte did not name a side. Nothing can be booked from it.
    if(e.side != 'B' && e.side != 'S') {
        return;
    }

    //An add reusing a live id would strand the old order in its queue
    if(orders_.find(e.order_id) != orders_.end()) {
        cancel(e.order_id);
    }

    NewOrder n;
    n.symbol = e.symbol;
    n.side = e.side;
    n.size = e.size;
    n.price = e.price;
    n.type = LIMIT;
    n.ts_ns = e.has_ts ? e.ts_ns : 0;
    n.client_id = e.order_id;

    ExecReport r = place(n, e.order_id);

    if(r.filled > 0) {
        crossed_adds_++;
        crossed_add_shares_ += r.filled;
    }
}


//Priority kept is a resize in place. Priority lost is a cancel and a replace, and the
//replacement goes back through matching like any other order - at its new price, against
//a book that may have drifted, it can now cross.
//
//The feed states the intent in bit 0 of the flags byte rather than leaving it to be
//inferred; the bit is read directly so this does not depend on a string.
void ExchangeBook::on_modify(const Event& e) {
    if(!e.has_order_id) {
        return;
    }

    unordered_map<int64_t, Order>::iterator it = orders_.find(e.order_id);

    if(it == orders_.end()) {
        orphans_++;
        return;
    }

    Order& o = it->second;

    int64_t new_price = e.has_price ? e.price : o.price;
    uint32_t new_size = e.has_size ? e.size : o.size;

    bool maintain = (e.flags & 1) != 0;
    bool shrinking = new_size <= o.size;

    if(maintain && shrinking && new_price == o.price) {
        uint32_t gone = o.size - new_size;

        resting_shares_ -= gone;
        cancelled_shares_ += gone;
        o.size = new_size;

        if(o.size == 0) {
            unordered_map<string, SymbolBook>::iterator b = books_.find(o.symbol);

            if(b != books_.end()) {
                dequeue(b->second, o);
            }

            orders_.erase(it);
        }

        return;
    }

    //Read what is needed before the cancel, which invalidates both it and o
    string symbol = o.symbol;
    char side = o.side;

    cancel(e.order_id);

    if(new_size == 0) {
        return;
    }

    NewOrder n;
    n.symbol = symbol;
    n.side = side;
    n.size = new_size;
    n.price = new_price;
    n.type = LIMIT;
    n.ts_ns = e.has_ts ? e.ts_ns : 0;
    n.client_id = e.order_id;

    ExecReport r = place(n, e.order_id);

    if(r.filled > 0) {
        crossed_modifies_++;
        crossed_modify_shares_ += r.filled;
    }
}


//Wipe one instrument. The id index has to be swept too, or cancels for wiped orders read
//as orphans and inflate the drift meter with something that is not drift.
void ExchangeBook::clear_symbol(const string& symbol) {
    unordered_map<string, SymbolBook>::iterator b = books_.find(symbol);

    if(b == books_.end()) {
        return;
    }

    unordered_map<int64_t, Order>::iterator it = orders_.begin();

    while(it != orders_.end()) {
        if(it->second.symbol == symbol) {
            resting_shares_ -= it->second.size;
            cancelled_shares_ += it->second.size;
            it = orders_.erase(it);

        } else {
            ++it;
        }
    }

    books_.erase(b);
}


//The feed as order flow. The one line that separates this book from every other one in the
//directory is `case 'L'`: an execution is IEX's answer, and this book is answering for
//itself, so taking it as input would remove the same shares twice.
void ExchangeBook::apply(const Event& e) {
    if(retain_) {
        LogEntry le;
        le.is_order = false;
        le.event = e;
        log_.push_back(le);
    }

    switch(e.kind) {
        case 'a': on_add(e); break;

        case 'R':
            if(e.has_order_id) {
                cancel(e.order_id);
            }
            break;

        case 'M': on_modify(e); break;

        //Dropped, deliberately and loudly. The count is how many matching decisions this
        //book took over from the feed, which is exactly the surface it can be wrong on.
        case 'L': ignored_executes_++; break;

        //A trade report names a trade, not a resting order, so the book does not move
        case 'T': break;

        case 'C': clear_symbol(e.symbol); break;

        //Administrative and reference messages carry no book effect
        default: break;
    }
}


TopOfBook ExchangeBook::get_top_of_book(const string& symbol) const {
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


void ExchangeBook::get_depth(const string& symbol, char side, size_t n,
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


void ExchangeBook::get_all_tops(vector<NamedTop>& out) const {
    out.clear();
    out.reserve(books_.size());

    for(unordered_map<string, SymbolBook>::const_iterator b = books_.begin();
        b != books_.end(); ++b) {
        out.push_back(make_pair(b->first, get_top_of_book(b->first)));
    }
}


//The read-only walk, kept alongside the mutating one so this book still answers the
//contract and can still be graded by matchcheck. Unbounded and non-destructive, exactly
//as NaiveBook's is - the limit price lives on NewOrder, not here.
template <class Levels>
static void probe_levels(const Levels& levels,
                     const unordered_map<int64_t, Order>& orders,
                     uint32_t size, vector<Fill>& out) {
    uint32_t left = size;

    for(typename Levels::const_iterator lv = levels.begin();
        lv != levels.end() && left > 0; ++lv) {

        for(list<int64_t>::const_iterator i = lv->second.begin();
            i != lv->second.end() && left > 0; ++i) {

            unordered_map<int64_t, Order>::const_iterator o = orders.find(*i);

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


void ExchangeBook::match(const string& symbol, char side, uint32_t size,
                     vector<Fill>& out) const {
    out.clear();

    unordered_map<string, SymbolBook>::const_iterator b = books_.find(symbol);

    if(b == books_.end()) {
        return;
    }

    if(side == 'B') {
        probe_levels(b->second.asks, orders_, size, out);
    } else {
        probe_levels(b->second.bids, orders_, size, out);
    }
}


char ExchangeBook::get_order_side(int64_t order_id) const {
    unordered_map<int64_t, Order>::const_iterator it = orders_.find(order_id);
    return (it == orders_.end()) ? 0 : it->second.side;
}


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


Spread ExchangeBook::get_spread(const string& symbol) const {
    return spread_from(get_top_of_book(symbol));
}


long long ExchangeBook::audit_resting_shares() const {
    long long total = 0;

    for(unordered_map<int64_t, Order>::const_iterator it = orders_.begin();
        it != orders_.end(); ++it) {
        total += it->second.size;
    }

    return total;
}


bool ExchangeBook::conserved() const {
    long long accounted = traded_taker_shares_ + traded_maker_shares_
                        + cancelled_shares_ + resting_shares_;

    //Both halves. The identity catches shares appearing or vanishing; the audit catches
    //the running counter drifting from the book it claims to describe, which is a
    //different bug and would otherwise hide inside a balanced identity.
    return accounted == submitted_shares_
        && resting_shares_ == audit_resting_shares();
}


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


//Feed messages only. EventQuery filters on Event fields and an injected order is not an
//Event, so returning one would mean inventing a kind character and a sequence number for
//something that never came off a wire.
void ExchangeBook::get_events(const EventQuery& q, vector<Event>& out) const {
    out.clear();

    for(size_t i = 0; i < log_.size(); i++) {
        if(log_[i].is_order) {
            continue;
        }

        if(matches(log_[i].event, q)) {
            out.push_back(log_[i].event);
        }
    }
}


//Replay both input paths, in arrival order. Timestamps are not monotonic within a packet,
//so this cannot stop at the first entry past ts_ns; the whole log is scanned and
//everything at or below the cutoff is applied.
void ExchangeBook::replay_until(int64_t ts_ns, ExchangeBook& into) const {
    for(size_t i = 0; i < log_.size(); i++) {
        const LogEntry& le = log_[i];

        if(le.is_order) {
            if(le.order.ts_ns > ts_ns) {
                continue;
            }

            //client_id is the id the live book resolved, so this books the same order
            //under the same id rather than assigning a fresh one
            into.submit(le.order);

        } else {
            if(le.event.has_ts && le.event.ts_ns > ts_ns) {
                continue;
            }

            into.apply(le.event);
        }
    }
}


TopOfBook ExchangeBook::get_top_of_book_at(const string& symbol, int64_t ts_ns) const {
    ExchangeBook scratch(DROP_LOG);
    replay_until(ts_ns, scratch);
    return scratch.get_top_of_book(symbol);
}


Spread ExchangeBook::get_spread_at(const string& symbol, int64_t ts_ns) const {
    return spread_from(get_top_of_book_at(symbol, ts_ns));
}


//One replay, holding a book current as of each entry, emitting only where the top actually
//moved. Entries before the window still have to be applied - the book at from_ts_ns
//depends on all of them - they just do not emit.
void ExchangeBook::get_top_of_book_series(const string& symbol, int64_t from_ts_ns,
                                      int64_t to_ts_ns, vector<TopSample>& out) const {
    out.clear();

    ExchangeBook scratch(DROP_LOG);

    //prev tracks the top across the whole replay rather than from the window edge.
    //Restarting it at the edge makes the first in-window entry look like a change even
    //when it moved nothing, which is how an empty book emits a bogus leading sample.
    TopOfBook prev = scratch.get_top_of_book(symbol);

    for(size_t i = 0; i < log_.size(); i++) {
        const LogEntry& le = log_[i];

        int64_t ts = 0;
        bool has_ts = false;
        bool touches = false;

        if(le.is_order) {
            scratch.submit(le.order);
            ts = le.order.ts_ns;
            has_ts = true;
            touches = (symbol == le.order.symbol);

        } else {
            scratch.apply(le.event);
            ts = le.event.ts_ns;
            has_ts = le.event.has_ts;
            touches = le.event.has_symbol && symbol == le.event.symbol;
        }

        //Only something on this instrument can move its top of book
        if(!touches) {
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

        if(has_ts && ts >= from_ts_ns && ts < to_ts_ns) {
            TopSample s;
            s.ts_ns = ts;
            s.top = now;
            out.push_back(s);
        }
    }
}


//Built on the top of book series rather than on a second replay, then thinned again: a
//size change on either side moves the top without moving the spread.
void ExchangeBook::get_spread_series(const string& symbol, int64_t from_ts_ns,
                                 int64_t to_ts_ns, vector<SpreadSample>& out) const {
    out.clear();

    vector<TopSample> tops;
    get_top_of_book_series(symbol, from_ts_ns, to_ts_ns, tops);

    Spread prev;
    prev.valid = false;
    prev.bid_price = 0;
    prev.ask_price = 0;
    prev.width = 0;
    bool have_prev = false;

    for(size_t i = 0; i < tops.size(); i++) {
        Spread s = spread_from(tops[i].top);

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


//Prices only, so this stays O(symbols) rather than walking every resting order
int64_t ExchangeBook::get_checksum() const {
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
