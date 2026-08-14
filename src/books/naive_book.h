//Iteration 1: the obvious implementation, written to be read rather than to be fast.
//
//  symbol   -> hash map to a per instrument book
//  price    -> ordered map to the queue resting at that price
//  order id -> hash map to the order, holding an iterator into its queue
//
//Every later iteration is measured against this one, so it is deliberately made of the
//containers a first pass would reach for: std::map for price ordering, std::list for the
//time queue, std::unordered_map for the id index. The list iterator in each order is the
//one concession to sanity, since without it a cancel would be a linear scan of the level
//and the baseline would be slow for a reason that has nothing to do with the design.
//
//Holding many instruments at once, and letting them come and go mid run, falls out of the
//symbol map: an instrument appears on first use and is erased on a clear, so the set is
//never fixed and nothing has to be declared up front.

#ifndef BOOKS_NAIVE_BOOK_H
#define BOOKS_NAIVE_BOOK_H

#include "books/book.h"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace books {

//A resting order. `where` points at this order's slot in its price level queue, so a
//cancel is an erase rather than a search.
struct Order {
    int64_t id;
    int64_t price;
    uint32_t size;
    char side;
    std::string symbol;
    std::list<int64_t>::iterator where;
};

//Bids descend, asks ascend, so in both maps begin() is the inside of the book
typedef std::map<int64_t, std::list<int64_t>, std::greater<int64_t> > BidLevels;
typedef std::map<int64_t, std::list<int64_t>, std::less<int64_t> > AskLevels;

struct SymbolBook {
    BidLevels bids;
    AskLevels asks;

    //Latches false once the stream has lost messages touching this instrument. From that
    //point the book is no longer a faithful replay and its depth cannot be trusted.
    bool trusted;

    SymbolBook() : trusted(true) {}
};


//Whether a book keeps the raw stream. Spelled out rather than passed as a bare bool so
//the choice is readable where it is made, since it is not a tuning knob: DROP_LOG makes
//every historical query answer empty, and is only correct for a book nothing will ask.
enum Retention {
    RETAIN_LOG,
    DROP_LOG
};


class NaiveBook {
public:
    NaiveBook();

    //Retention off. The log is the larger half of this book's memory on a real capture -
    //an Event is 128 bytes, so a full session runs to gigabytes - and a caller replaying
    //a stream it already holds is otherwise paying for a second copy of it. Every method
    //under the history section returns empty on a book built this way.
    explicit NaiveBook(Retention r);

    //Ingest one decoded message. Messages that carry no book effect are dropped.
    void apply(const Event& e);

    //Present state
    TopOfBook get_top_of_book(const std::string& symbol) const;
    void get_depth(const std::string& symbol, char side, size_t n,
               std::vector<LevelView>& out) const;
    void get_all_tops(std::vector<NamedTop>& out) const;
    Spread get_spread(const std::string& symbol) const;

    //Consume resting liquidity on the far side, best price first and oldest first within
    //a price, until `size` shares are taken or the book runs dry. Does not mutate.
    void match(const std::string& symbol, char side, uint32_t size,
           std::vector<Fill>& out) const;

    //Side an order is resting on, or 0 if the id is not live. Not part of the contract:
    //a harness replaying real executions needs it to tell which way one ran, since an
    //execute message carries no side, but matching itself never asks.
    char get_order_side(int64_t order_id) const;

    //History. This book takes the cheap-ingest side of the tradeoff: it keeps the raw
    //event log and answers by replaying it, so apply() pays only an append and the cost
    //lands on the query instead. Every point query is a full replay from the start of
    //the stream, which is linear and unapologetic; a later book that maintains a change
    //series as events arrive should beat it on these by a wide margin, and lose on
    //apply(). That contrast is the point of keeping this one.
    void get_events(const EventQuery& q, std::vector<Event>& out) const;
    TopOfBook get_top_of_book_at(const std::string& symbol, int64_t ts_ns) const;
    Spread get_spread_at(const std::string& symbol, int64_t ts_ns) const;
    void get_top_of_book_series(const std::string& symbol, int64_t from_ts_ns,
                            int64_t to_ts_ns, std::vector<TopSample>& out) const;
    void get_spread_series(const std::string& symbol, int64_t from_ts_ns, int64_t to_ts_ns,
                       std::vector<SpreadSample>& out) const;

    //Cheap digest of book state. Not a correctness check, see book.h.
    int64_t get_checksum() const;

    size_t get_symbol_count() const { return books_.size(); }
    size_t get_order_count() const { return orders_.size(); }
    size_t get_log_size() const { return log_.size(); }

    //Sum of every live order's size, walked the long way. Added for the drift harness,
    //which compares this book's resting shares against an exchange's; read only, O(orders),
    //and never called from a timed region, so it does not affect any measured figure.
    long long audit_resting_shares() const;

    //Messages naming an order this book never saw. In a mirror this is a cold start
    //artifact that goes flat and stays there, which is what makes it the baseline the
    //exchange's climbing count is read against.
    long long orphans() const { return orphans_; }

private:
    //Replay every event at or below ts_ns into a fresh book, in stream order
    void replay_until(int64_t ts_ns, NaiveBook& into) const;

    void add(const Event& e);
    void remove(int64_t order_id);
    void modify(const Event& e);
    void execute(const Event& e);
    void clear_symbol(const std::string& symbol);

    //Put an order at the back of its price level queue, creating the level if new
    void enqueue(SymbolBook& sb, Order& o);

    //Take an order out of its price level queue, dropping the level if it empties
    void dequeue(SymbolBook& sb, const Order& o);

    std::unordered_map<std::string, SymbolBook> books_;
    std::unordered_map<int64_t, Order> orders_;

    //The retained stream, in ingestion order, which is what makes the historical
    //queries answerable at all. Held in full: this book does no summarizing.
    std::vector<Event> log_;
    bool retain_;

    //Messages whose order id was never seen, which is normal at the head of a capture
    //that starts mid session, and a symptom of a dropped add after a gap.
    long long orphans_;
};

} //namespace books

#endif
