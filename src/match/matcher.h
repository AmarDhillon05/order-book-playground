//Grading a book's matching against what IEX actually did.
//
//The matching itself is not here. It belongs to the book - see BookModel::match in
//books/book.h - because the walk is where a storage strategy shows itself, and a single
//shared implementation over some exposed queue would measure one backend's walk while
//pretending to measure all of them.
//
//What is left is the part that is genuinely common: deciding whether an answer is right,
//and counting how the answers came out. Those do not vary by backend and should not be
//written twice.

#ifndef MATCH_MATCHER_H
#define MATCH_MATCHER_H

#include "books/book.h"
#include "match/taker.h"

#include <cstdint>
#include <string>
#include <vector>

namespace match {

//How one comparison came out. Ordered by how much they say: EXACT down to SKIPPED.
enum Verdict {
    EXACT,                          //Same orders, same sequence, same sizes

    //Every disagreement below is a real finding, not an error. The interesting question
    //is which kind dominates, since they have different causes.
    WRONG_ORDERS,                   //A different set or sequence of orders was consumed
    WRONG_SIZES,                    //Right orders, wrong split of shares between them
    BOOK_TOO_THIN,                  //Ran out of resting orders before filling the taker

    //Not comparable, and counted apart so they cannot flatter the pass rate
    SKIPPED_HIDDEN,                 //Non-displayed liquidity was part of this sweep
    SKIPPED_UNKNOWN_SIDE            //No victim was resting, so the side is unresolvable
};


struct MatchStats {
    long long takers;               //Groups reached during the replay
    long long sweeps;               //Of those, the ones consuming more than one order

    //Sweeps carry the test. A single fill taker only asks which order is at the front of
    //the queue; a sweep asks for the whole consumption sequence across levels, which is
    //the actual price-time priority claim. Broken out so a headline pass rate cannot rest
    //on the easy majority.
    long long sweeps_comparable;
    long long sweeps_exact;

    long long exact;
    long long wrong_orders;
    long long wrong_sizes;
    long long book_too_thin;
    long long skipped_hidden;
    long long skipped_unknown_side;

    long long shares_matched;       //Shares the matcher placed correctly
    long long shares_seen;          //Shares in every comparable taker

    //Cancels pulled forward ahead of a match, see Taker::presteps
    long long presteps;

    MatchStats()
        : takers(0), sweeps(0), sweeps_comparable(0), sweeps_exact(0), exact(0),
          wrong_orders(0), wrong_sizes(0), book_too_thin(0), skipped_hidden(0),
          skipped_unknown_side(0), shares_matched(0), shares_seen(0), presteps(0) {}
};


//Grade one comparison.
//
//Order ids and sizes only. Price is never compared: an execution reports the trade price,
//which price improvement and price sliding can move off the resting order's booking
//price, so a price difference is the feed being honest rather than the matcher being
//wrong. Sequence counts as much as membership, since the sequence IS the time priority
//claim being tested.
Verdict grade(const std::vector<Fill>& got, const std::vector<Fill>& expected);


//Human name for a verdict, for the report
const char* verdict_name(Verdict v);

} //namespace match

#endif
