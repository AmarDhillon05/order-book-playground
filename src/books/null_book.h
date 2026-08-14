//The control. Satisfies the book contract and stores nothing.
//
//Its time is the floor: the loop, the event fetch, and the call, with no storage
//underneath. Subtract it and what is left is what a real book's data structure actually
//costs. If a real book ever measures close to this, the measurement is wrong before the
//book is.
//
//The query methods are here to meet the contract, not to answer anything. A book with no
//state has no top of book and no history, so they all report empty. That is deliberate:
//the control must not retain the stream, or it would stop being a floor and start
//measuring the cost of retention.

#ifndef BOOKS_NULL_BOOK_H
#define BOOKS_NULL_BOOK_H

#include "books/book.h"

namespace books {

class NullBook {
public:
    NullBook() : seen_(0) {}

    void apply(const Event& e) {
        //Touch the event so the read is not optimized away, but keep it to one add
        seen_ += e.seq;
    }

    TopOfBook get_top_of_book(const std::string&) const {
        TopOfBook t;
        t.bid_price = 0;
        t.bid_size = 0;
        t.ask_price = 0;
        t.ask_size = 0;
        t.has_bid = false;
        t.has_ask = false;
        return t;
    }

    void get_depth(const std::string&, char, size_t, std::vector<LevelView>& out) const {
        out.clear();
    }

    void get_all_tops(std::vector<NamedTop>& out) const {
        out.clear();
    }

    Spread get_spread(const std::string&) const { return empty_spread(); }

    //No resting liquidity to consume, so every incoming order finds an empty book
    void match(const std::string&, char, uint32_t, std::vector<Fill>& out) const {
        out.clear();
    }

    void get_events(const EventQuery&, std::vector<Event>& out) const {
        out.clear();
    }

    TopOfBook get_top_of_book_at(const std::string& s, int64_t) const {
        return get_top_of_book(s);
    }

    Spread get_spread_at(const std::string&, int64_t) const { return empty_spread(); }

    void get_top_of_book_series(const std::string&, int64_t, int64_t,
                            std::vector<TopSample>& out) const {
        out.clear();
    }

    void get_spread_series(const std::string&, int64_t, int64_t,
                       std::vector<SpreadSample>& out) const {
        out.clear();
    }

    int64_t get_checksum() const { return seen_; }

private:
    static Spread empty_spread() {
        Spread s;
        s.bid_price = 0;
        s.ask_price = 0;
        s.width = 0;
        s.valid = false;
        return s;
    }

    int64_t seen_;
};

} //namespace books

#endif
