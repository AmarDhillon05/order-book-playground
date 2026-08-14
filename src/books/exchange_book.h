//An order book that decides its own matches.
//
//Every other book in this directory is a mirror: it reads what IEX did and reproduces it.
//This one is an exchange. Adds, cancels and modifies are treated as client requests rather
//than as state updates, incoming orders are matched against resting liquidity here, and
//the fills are ours. IEX's execute messages are deliberately DROPPED on ingest - they are
//the comparison target, never an input, and applying them would remove the same shares
//twice, once because IEX said so and once because we decided so.
//
//That single change is what makes the book able to be wrong. A mirror cannot diverge; it
//is told the answer. An exchange makes a decision per incoming order, and once one of
//those decisions differs from IEX's the two books hold different orders and nothing pulls
//them back together. Measuring how far and how fast they separate is the point - see
//bench/drift.h - and this book carries the two counters that detect it from the inside:
//
//  crossed_adds     IEX published this order as RESTING, so at IEX it did not cross the
//                   book. If it crosses ours, our book holds liquidity theirs did not.
//                   That is drift, observed directly, with no second book to compare to.
//
//  orphans          a cancel or modify for an order we no longer have. In a mirror this
//                   is flat after the cold start. Here it climbs every time we wrongly
//                   consumed an order that IEX left resting.
//
//What is NOT modelled, and cannot be from this feed: IEX's non-displayed liquidity
//(midpoint pegs, D-Limit), the 350 us speed bump, and price sliding. Those are the known
//causes of drift rather than bugs in it, and the honest claim this book supports is
//"price-time priority, implemented correctly, matching real order flow" - never
//"a simulation of IEX".
//
//The contract in books/book.h is untouched. match() is still const and still answers
//"what would happen" without moving anything, so matchcheck grades this book exactly as
//it grades the mirror. submit() is the mutating path and is additional, not a
//replacement. Keeping them separate is what lets the same binary report a resynced pass
//rate and a free-running drift rate side by side.

#ifndef BOOKS_EXCHANGE_BOOK_H
#define BOOKS_EXCHANGE_BOOK_H

#include "books/book.h"
#include "books/naive_book.h"       //for books::Retention, and Order/level typedefs

#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace books {

//What an incoming order asks for when it cannot be filled in full.
enum OrderType {
    //Rest the remainder on the book. What every feed add becomes.
    LIMIT,

    //No price bound: take whatever is there at any price. The remainder is cancelled.
    //Reconstructed takers arrive as these, because the feed never publishes a taker's
    //limit price and reconstructing it from the worst price the sweep reached would hand
    //the matcher the depth it is supposed to be predicting. See match/taker.h.
    MARKET,

    //Priced like a limit order, but the remainder is cancelled instead of rested
    IOC
};


//A client request. Distinct from Event on purpose: an Event is something that already
//happened on a wire, this is something being asked for.
struct NewOrder {
    std::string symbol;
    char side;                      //'B' or 'S'
    uint32_t size;
    int64_t price;                  //Ignored when type is MARKET
    OrderType type;
    int64_t ts_ns;

    //The id to book this order under, or 0 to have the exchange assign one.
    //
    //Feed replay passes IEX's own order id, because a later cancel names that id and
    //would otherwise find nothing. Assigned ids count DOWN from -1 so that an injected
    //order can never collide with a feed id, all of which are large and positive.
    int64_t client_id;

    NewOrder()
        : side(0), size(0), price(0), type(LIMIT), ts_ns(0), client_id(0) {}
};


//What happened to one submitted order. filled + resting + cancelled == size, always;
//that identity is the cheapest real test of a matching engine and is asserted per submit
//by way of the share counters below.
struct ExecReport {
    int64_t order_id;               //As booked, whether client supplied or assigned
    uint32_t filled;                //Traded immediately against resting liquidity
    uint32_t resting;               //Now on the book
    uint32_t cancelled;             //Went away unfilled
    std::vector<Fill> fills;        //What it consumed, in the order consumed

    ExecReport() : order_id(0), filled(0), resting(0), cancelled(0) {}
};


//One entry in the retained stream.
//
//An exchange has two input paths, and a historical query has to replay both in the order
//they arrived or it answers about a book that never existed. A log of Events alone would
//reconstruct the book as feed replay alone would have built it - with no aggressive flow
//in it at all - and would return that confidently as the state at a timestamp.
//
//Both members are held inline rather than in a union. It wastes space on the unused half,
//and the log is already this book's largest allocation, but a tagged union here would buy
//bytes at the cost of the one part of the file that has to be obviously correct.
struct LogEntry {
    bool is_order;                  //true: a submitted NewOrder. false: a feed Event.
    Event event;
    NewOrder order;

    LogEntry() : is_order(false) {}
};


class ExchangeBook {
public:
    ExchangeBook();

    //Retention off. The log is the larger half of this book's memory and a caller
    //replaying a stream it already holds would otherwise pay for a second copy. Every
    //method under the history section returns empty on a book built this way.
    explicit ExchangeBook(Retention r);

    //--- the contract in books/book.h -------------------------------------------------

    //Ingest one feed message, as ORDER FLOW rather than as a state update:
    //
    //  'a' add     submitted as a LIMIT order, which may cross and fill on arrival
    //  'R' delete  cancelled
    //  'M' modify  priority kept is a resize in place; priority lost is a cancel and a
    //              replace, and the replacement is matched like any other order
    //  'L' execute DROPPED. This book decides its own executions.
    //  'T' print   ignored, as everywhere: it names a trade, not a resting order
    //  'C' clear   wipes the instrument
    //
    //Everything else carries no book effect and is retained without being acted on.
    void apply(const Event& e);

    TopOfBook get_top_of_book(const std::string& symbol) const;
    void get_depth(const std::string& symbol, char side, size_t n,
               std::vector<LevelView>& out) const;
    void get_all_tops(std::vector<NamedTop>& out) const;
    Spread get_spread(const std::string& symbol) const;

    //Unchanged from the contract: const, and does not move the book. This is the probe,
    //not the exchange. submit() is the one that trades.
    void match(const std::string& symbol, char side, uint32_t size,
           std::vector<Fill>& out) const;

    void get_events(const EventQuery& q, std::vector<Event>& out) const;
    TopOfBook get_top_of_book_at(const std::string& symbol, int64_t ts_ns) const;
    Spread get_spread_at(const std::string& symbol, int64_t ts_ns) const;
    void get_top_of_book_series(const std::string& symbol, int64_t from_ts_ns,
                            int64_t to_ts_ns, std::vector<TopSample>& out) const;
    void get_spread_series(const std::string& symbol, int64_t from_ts_ns, int64_t to_ts_ns,
                       std::vector<SpreadSample>& out) const;

    int64_t get_checksum() const;

    //--- the exchange ------------------------------------------------------------------

    //Submit an order. Matches it against the far side, best price first and oldest first
    //within a price, stopping at the limit if it has one; rests or cancels whatever is
    //left according to the type. THIS MUTATES: consumed orders are decremented and
    //removed for real.
    ExecReport submit(const NewOrder& n);

    //Pull a resting order. False if the id is not live, which is counted as an orphan.
    bool cancel(int64_t order_id);

    //Side an order rests on, or 0 if the id is not live
    char get_order_side(int64_t order_id) const;

    //--- drift, seen from the inside ---------------------------------------------------

    //An add or a priority-losing modify that crossed this book. The feed published it as
    //resting, so at IEX it crossed nothing: every one of these is a direct observation
    //that this book held liquidity IEX's did not.
    long long crossed_adds() const { return crossed_adds_; }
    long long crossed_add_shares() const { return crossed_add_shares_; }
    long long crossed_modifies() const { return crossed_modifies_; }
    long long crossed_modify_shares() const { return crossed_modify_shares_; }

    //Cancels and modifies naming an order this book does not have. Normal at the head of
    //a capture that starts mid session; after that, a drift meter.
    long long orphans() const { return orphans_; }

    //IEX executions dropped on ingest. Their count is the number of matching decisions
    //this book took over from the feed.
    long long ignored_executes() const { return ignored_executes_; }

    //--- conservation of shares --------------------------------------------------------
    //
    //Every share ever submitted is, at any instant, in exactly one of four places: it
    //traded as the incoming order, it traded as the resting order, it was cancelled, or
    //it is still on the book. Nothing is created and nothing vanishes.
    //
    //  submitted == traded_taker + traded_maker + cancelled + resting
    //
    //This is the strongest cheap test a matching engine has, and it is independent of
    //IEX entirely - it holds or it does not, no oracle required. A failure here is a bug
    //in this file, never drift.

    long long submitted_orders() const { return submitted_orders_; }
    long long submitted_shares() const { return submitted_shares_; }
    long long traded_taker_shares() const { return traded_taker_shares_; }
    long long traded_maker_shares() const { return traded_maker_shares_; }
    long long cancelled_shares() const { return cancelled_shares_; }
    long long resting_shares() const { return resting_shares_; }
    long long fills() const { return fills_; }

    //Sum the sizes of every live order the long way. O(orders), for auditing the running
    //resting_shares_ counter rather than for trusting it.
    long long audit_resting_shares() const;

    //Both halves of the check: the identity above holds, and the running counter agrees
    //with the walk. Never called inside a timed region.
    bool conserved() const;

    size_t get_symbol_count() const { return books_.size(); }
    size_t get_order_count() const { return orders_.size(); }
    size_t get_log_size() const { return log_.size(); }

private:
    //Walk one side outward from the inside, consuming orders as it goes. Returns shares
    //taken. `bounded` false is a market order: no price stops it.
    template <class Levels>
    uint32_t consume(Levels& levels, char taker_side, uint32_t size,
                 bool bounded, int64_t limit, std::vector<Fill>& fills);

    //Match and then rest or cancel, under an id the caller has already resolved.
    //
    //Split out from submit() so that the feed path can place an order WITHOUT logging a
    //second time - apply() has already retained the Event that caused it, and logging the
    //derived NewOrder as well would replay every feed add twice.
    ExecReport place(const NewOrder& n, int64_t id);

    void on_add(const Event& e);
    void on_modify(const Event& e);
    void clear_symbol(const std::string& symbol);

    void enqueue(SymbolBook& sb, Order& o);
    void dequeue(SymbolBook& sb, const Order& o);

    //Replay every retained entry at or below ts_ns into a fresh book, in arrival order
    void replay_until(int64_t ts_ns, ExchangeBook& into) const;

    std::unordered_map<std::string, SymbolBook> books_;
    std::unordered_map<int64_t, Order> orders_;

    std::vector<LogEntry> log_;
    bool retain_;

    //Counts DOWN from -1, so an exchange assigned id can never collide with a feed id
    int64_t next_id_;

    long long crossed_adds_;
    long long crossed_add_shares_;
    long long crossed_modifies_;
    long long crossed_modify_shares_;
    long long orphans_;
    long long ignored_executes_;

    long long submitted_orders_;
    long long submitted_shares_;
    long long traded_taker_shares_;
    long long traded_maker_shares_;
    long long cancelled_shares_;
    long long resting_shares_;
    long long fills_;
};

} //namespace books

#endif
