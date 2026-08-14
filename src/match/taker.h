//Reconstructing the incoming orders IEX never published.
//
//DPLS reports what happened to resting orders and nothing about what hit them. An
//execution names its victim, its size and its price; the aggressor that caused it is not
//a message on the feed. But an incoming order that consumes several resting orders
//produces several executions stamped at the same instant, so grouping them recovers the
//order that was never sent: its side, its total size, and how deep it reached.
//
//That grouping is what makes matching testable at all. Each group splits cleanly in two:
//
//  input   the aggregate - timestamp, symbol, side, total displayed size
//  answer  the decomposition - which orders were hit, in what sequence, for how much
//
//The decomposition is exactly what price-time priority determines, so a matcher given
//only the aggregate has to reproduce it. Nothing in Taker::expected may reach the matcher
//or the test grades itself.
//
//Two things this cannot see, both of them structural rather than fixable:
//
//  Hidden liquidity. A group interleaved with a trade report is a sweep that also filled
//  against non-displayed orders. Those shares are real and are missing from the size,
//  so the reconstructed taker is the displayed residue of the real order, not the order.
//  Flagged rather than dropped, since how much it costs is worth measuring.
//
//  Order type and limit. IOC, pegs and the actual limit price are never published. The
//  limit especially is deliberately not reconstructed: it can be read off the worst price
//  the sweep reached, but that price is an outcome, and feeding it back in would tell the
//  matcher how deep to go. Depth is part of what is being predicted.

#ifndef MATCH_TAKER_H
#define MATCH_TAKER_H

#include "books/book.h"

#include <cstdint>
#include <string>
#include <vector>

namespace match {

//One reconstructed incoming order.
struct Taker {
    int64_t ts_ns;
    std::string symbol;

    //'B' or 'S', the side the aggressor was on, which is the opposite of the side its
    //victims were resting on. Left 0 by extraction: an execution carries no side, so this
    //can only be resolved against a book that still holds the orders.
    char side;

    uint32_t size;                  //Total displayed shares taken

    //Where this group starts in the event vector. The check has to run at exactly this
    //point in the replay, before any of the group's executions have touched the book.
    size_t first_index;

    //Cancels and modifies for this instrument that land inside the group's span, which
    //have to be applied before the match rather than at their stream position.
    //
    //A matching pass is atomic, so anything at the same instant that took liquidity away
    //must have been processed before it - had it not been, the taker would have consumed
    //that order instead of stepping over it. The feed interleaves these after the first
    //execution anyway, and matching against a book that still holds a cancelled order
    //reads as a priority failure when it is only a stale snapshot.
    //
    //Adds are deliberately not included. The same argument does not run backwards: an add
    //at the same instant may or may not have been visible to the taker, and nothing in
    //the feed says which, so it is left where the stream put it.
    std::vector<size_t> presteps;

    //A trade report for the same instrument and instant fell inside this group's span, so
    //part of the real order filled against liquidity the book cannot contain
    bool hidden;

    //The held-out answer, in the order the feed reported it. Fill comes from book.h, the
    //same type a book's match() produces, so the two are compared without conversion -
    //but note the price fields mean different things and must not be compared. This one
    //carries the trade price off the wire; a book fills in the resting order's booking
    //price, which price improvement can move away from it.
    std::vector<Fill> expected;
};


//Pass one. Walks the decoded stream and groups executions by (timestamp, symbol).
//
//Contiguity is not required, and must not be: exchange timestamps are not monotonic
//within a packet, so an unrelated message can land between two halves of one sweep. Runs
//are therefore closed by the timestamp moving on rather than by the next event differing,
//which costs a little buffering and recovers the roughly 2% of groups a contiguous scan
//would tear in half.
void extract_takers(const std::vector<Event>& events, std::vector<Taker>& out);

} //namespace match

#endif
