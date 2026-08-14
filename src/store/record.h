//One decoded message, as it lives on disk.
//
//Event cannot be the on-disk form and the reason is not stylistic. It carries a
//std::string and a pointer into the reader's message buffer, so it is neither trivially
//copyable nor meaningful once written; and at 128 bytes it is the whole memory ceiling
//this project measured - a full stream is ~6.5 GB in the event vector alone, and a book
//that retains it holds a second copy.
//
//LogRecord is the fixed 64 byte form. Two properties follow from being fixed width, and
//both are load bearing:
//
//  a record's position in a node is arithmetic, not a search. The i'th record of a page
//  is at header + i * RECORD_SIZE, so once a query has found the node it reaches the slot
//  in O(1) rather than scanning to it.
//
//  records never straddle a page boundary. 64 divides every power-of-two page size, so a
//  page holds a whole number of them and a read never has to stitch two pages together.
//
//WHAT IS DROPPED, AND WHY IT IS SAFE
//
//Event::detail is display text the decoder synthesizes from kind and flags - "halted",
//"maintain-priority", and so on. It is derived, not carried on the wire, and no book
//reads it: naive_book.cpp:133 makes the point explicitly, reading the priority bit out of
//flags rather than depending on the string. So it is regenerable and is not stored.
//to_event() leaves it empty, which costs a suffix on a formatted line and nothing else.
//
//Event::raw and raw_len point into the reader's single message buffer, which the next
//read overwrites. They cannot outlive the call that produced them and are not stored.

#ifndef STORE_RECORD_H
#define STORE_RECORD_H

#include "loader/dpls_reader.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace store {

//Every record is this wide, on disk and in memory. Changing it invalidates every file
//ever written, which is why the spare bytes below exist.
const size_t RECORD_SIZE = 64;

//Which optional fields this record actually carries. Event spends a bool on each; here
//they are bits, because the difference is 6 bytes per record against a stream of tens of
//millions of them.
//
//Absent is not the same as zero. A size of 0 is a real value on a level-removed message,
//so a reader that treats a missing field as 0 will silently invent one.
enum RecordPresence {
    HAS_TS       = 1 << 0,
    HAS_SYMBOL   = 1 << 1,
    HAS_ORDER_ID = 1 << 2,
    HAS_SIZE     = 1 << 3,
    HAS_PRICE    = 1 << 4,
    HAS_TRADE_ID = 1 << 5
};


//Laid out widest first so every 8 byte field lands naturally aligned with no interior
//padding, and the trailing bytes are explicit rather than whatever the compiler inserts.
//Implicit padding would be uninitialized on write, which makes two logically identical
//files differ byte for byte and breaks any checksum taken over a page.
struct LogRecord {
    int64_t seq;
    int64_t ts_ns;                  //Exchange clock, already UTC. See the TAI note in
                                    //loader/dpls_reader.h - the capture clock is not this
    int64_t order_id;
    int64_t price;                  //Fixed point, 4 decimals. Never a float
    int64_t trade_id;

    uint32_t size;

    //8 bytes exactly, NUL padded rather than NUL terminated: a full 8 character symbol
    //fills the field with no room for a terminator. Event::symbol is 9 bytes for exactly
    //that reason, so the conversion has somewhere to put one.
    char symbol[8];

    char kind;
    uint8_t flags;
    char side;                      //'B', 'S', '?' or 0 when the type has no side
    uint8_t present;                //RecordPresence bits

    //Spare, and deliberately not removed to save space. A future field added here keeps
    //RECORD_SIZE at 64 and every existing file readable; a field added past it changes
    //the stride, and every position calculation in every database silently addresses the
    //wrong slot. Zeroed on write so pages stay byte-comparable.
    uint8_t reserved[8];
};

static_assert(sizeof(LogRecord) == RECORD_SIZE,
    "LogRecord must stay 64 bytes: page layout and O(1) slot addressing both assume it");

static_assert(std::is_trivially_copyable<LogRecord>::value,
    "LogRecord is written to disk with memcpy and must stay trivially copyable");


//Decoded message to stored record. Fields the message does not carry are zeroed AND
//flagged absent, so a record round trips rather than acquiring values it never had.
void to_record(const Event& e, LogRecord& out);

//Stored record back to a decoded message. detail comes back empty and raw comes back
//null, per the note at the top of this file.
void to_event(const LogRecord& r, Event& out);

} //namespace store

#endif
