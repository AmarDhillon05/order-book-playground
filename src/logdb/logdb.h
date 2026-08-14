//The contract every log database satisfies.
//
//Same shape as books/book.h and for the same reasons: LogDbModel is the whole surface
//written out as declarations, never instantiated and never inherited, so the required API
//reads off one screen. LogDbContract<D> then checks a real database against it at compile
//time, so a database missing a method fails the build naming the method.
//
//WHAT A LOG DATABASE IS FOR
//
//It is the answer to the five historical methods in books/book.h. NaiveBook answers those
//by keeping the whole stream in a vector and replaying it, which is why a full session
//holds a second copy of the capture at 128 bytes an event and why retention - not the
//data structure - is this project's measured memory ceiling. Everything here exists to
//move that copy onto disk and read back only the part a query needs.
//
//APPEND ONLY, DELIBERATELY
//
//There is no update and no erase. A market data log is a record of what happened, and
//nothing that happened stops having happened - a cancel is another record, not the
//removal of the add. The live book is where state is mutated, and it has its own paged
//store; conflating the two would put a random write path into a structure whose whole
//advantage is that it does not have one.
//
//WHY BATCH SIZE IS ASKED FOR RATHER THAN CONFIGURED
//
//batch_size() is a question put TO the database, on every batch, and the caller does not
//cache the answer. A fixed-size database returns a constant and the call folds away. The
//adaptive one returns whatever its read-amplification metric last decided, so the batch
//being filled right now can be a different size from the one before it. Making this a
//method rather than a constructor argument is what lets the two share every other line.
//
//THE TWO COUNTERS ARE NOT INSTRUMENTATION
//
//records_scanned() and records_returned() are part of the contract, not a debug extra.
//Their ratio is record-level read amplification - how much a query had to look at against
//how much it wanted - and it is simultaneously the adaptive variant's control input and
//the number this whole comparison is trying to produce. A database that does not report
//it cannot be compared with one that does.

#ifndef LOGDB_LOGDB_H
#define LOGDB_LOGDB_H

#include "store/batch.h"
#include "store/pager.h"
#include "store/record.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace logdb {

//A position in the stream: a node, and a slot inside it.
//
//This is the thing seek() computes and next() walks, and it is why finding a record is
//not a search. The node is reached through the database's own structure - a skip list
//hop, a tree descent - and the slot inside it is arithmetic, because records are fixed
//width. So the cost of a point lookup is the cost of finding the NODE; everything after
//that is an offset.
struct Cursor {
    store::PageId page;
    int32_t slot;                   //Index of the next record to yield within the page

    //False once the walk has run off the end of the stream. Kept as a flag rather than
    //spelled as page == NULL_PAGE so that a cursor can also be invalidated by a rebalance
    //moving the node out from under it.
    bool valid;

    Cursor() : page(store::NULL_PAGE), slot(0), valid(false) {}
};


//The contract, in full. Declarations only.
class LogDbModel {
public:
    //Records this database wants in the batch being filled right now. Asked per batch;
    //may differ from the last answer.
    size_t batch_size() const;

    //Append one record to the open batch, flushing it to a node when it fills. Records
    //arrive in stream order - the same order books/book.h::apply() guarantees - so a
    //database may assume it never sees the stream backwards.
    void append(const store::LogRecord& r);

    //Close out a partly filled batch. Called at the end of a run, and by anything that
    //needs the stream on disk before reading it back. A database with nothing buffered
    //does nothing.
    void flush();

    //Position at the first record at or after ts_ns.
    //
    //Timestamps are not monotonic within a packet, so "at or after" is by stream position
    //and not a guarantee about every record beyond it - a scan can still see a slightly
    //earlier time a few records in. Callers filtering a window must test each record
    //rather than assuming the cursor bounds it.
    Cursor seek(int64_t ts_ns) const;

    //Yield the record at the cursor and advance. False at end of stream, following node
    //links across page and file boundaries without the caller knowing there were any.
    bool next(Cursor& c, store::LogRecord& out) const;

    //Every record in [from_ts_ns, to_ts_ns), in stream order. Clears out first. Half open
    //so two adjacent windows tile the stream without either dropping or repeating the
    //boundary, matching EventQuery in books/book.h.
    void scan(int64_t from_ts_ns, int64_t to_ts_ns,
          std::vector<store::LogRecord>& out) const;

    //Aggregate over the same half open window, WITHOUT materialising the records.
    //
    //This is where the structures separate. A node fully inside the window contributes
    //its header summary and is never opened; only the two partial nodes at the ends have
    //to be read. A B+ tree can go further and fold an interior node covering many leaves.
    //A database that answers this by scanning is allowed - it is still correct - and how
    //far its cost sits above the ideal is exactly the result being measured.
    store::BatchSummary summarize(int64_t from_ts_ns, int64_t to_ts_ns) const;

    //Work deferred to a quiet point: rebalancing, resizing, compaction.
    //
    //Never called from a read or a write. The adaptive variant reshapes nodes here and
    //only here, because doing it mid-query invalidates the cursors of the query asking
    //for it, and because a rebalance charged to whichever unlucky read triggered it makes
    //every latency distribution a measurement of when maintenance happened to fire. A
    //database with nothing to do does nothing, which is the fixed-size case.
    void maintain();

    //Records read to answer everything asked so far, and records actually handed back.
    //Their ratio is the read amplification described at the top of this file.
    int64_t records_scanned() const;
    int64_t records_returned() const;

    //Nodes written. The other half of the cost: a small batch size buys a tight scan and
    //pays for it here, and the crossover between the two is what the adaptive variant is
    //hunting for.
    int64_t nodes_written() const;

private:
    LogDbModel();
};


//Detection of each method, by call expression and return type. A database taking its
//arguments by different but compatible types still passes: what is pinned down is the
//call the harness makes and the type it gets back.
namespace logdb_detail {

template <class D, class = void>
struct has_batch_size : std::false_type {};

template <class D>
struct has_batch_size<D, std::void_t<decltype(std::declval<const D&>().batch_size())> >
    : std::is_same<size_t, decltype(std::declval<const D&>().batch_size())> {};


template <class D, class = void>
struct has_append : std::false_type {};

template <class D>
struct has_append<D, std::void_t<decltype(
        std::declval<D&>().append(std::declval<const store::LogRecord&>()))> >
    : std::is_same<void, decltype(
        std::declval<D&>().append(std::declval<const store::LogRecord&>()))> {};


template <class D, class = void>
struct has_flush : std::false_type {};

template <class D>
struct has_flush<D, std::void_t<decltype(std::declval<D&>().flush())> >
    : std::is_same<void, decltype(std::declval<D&>().flush())> {};


template <class D, class = void>
struct has_seek : std::false_type {};

template <class D>
struct has_seek<D, std::void_t<decltype(std::declval<const D&>().seek(int64_t(0)))> >
    : std::is_same<Cursor, decltype(std::declval<const D&>().seek(int64_t(0)))> {};


template <class D, class = void>
struct has_next : std::false_type {};

template <class D>
struct has_next<D, std::void_t<decltype(
        std::declval<const D&>().next(std::declval<Cursor&>(),
                                  std::declval<store::LogRecord&>()))> >
    : std::is_same<bool, decltype(
        std::declval<const D&>().next(std::declval<Cursor&>(),
                                  std::declval<store::LogRecord&>()))> {};


template <class D, class = void>
struct has_scan : std::false_type {};

template <class D>
struct has_scan<D, std::void_t<decltype(
        std::declval<const D&>().scan(int64_t(0), int64_t(0),
                                  std::declval<std::vector<store::LogRecord>&>()))> >
    : std::true_type {};


template <class D, class = void>
struct has_summarize : std::false_type {};

template <class D>
struct has_summarize<D, std::void_t<decltype(
        std::declval<const D&>().summarize(int64_t(0), int64_t(0)))> >
    : std::is_same<store::BatchSummary, decltype(
        std::declval<const D&>().summarize(int64_t(0), int64_t(0)))> {};


template <class D, class = void>
struct has_maintain : std::false_type {};

template <class D>
struct has_maintain<D, std::void_t<decltype(std::declval<D&>().maintain())> >
    : std::is_same<void, decltype(std::declval<D&>().maintain())> {};


template <class D, class = void>
struct has_counters : std::false_type {};

template <class D>
struct has_counters<D, std::void_t<decltype(std::declval<const D&>().records_scanned()),
                               decltype(std::declval<const D&>().records_returned()),
                               decltype(std::declval<const D&>().nodes_written())> >
    : std::true_type {};

} //namespace logdb_detail


//Instantiate this against a database to check it. Every registered backend is checked
//before it is timed, so a nonconforming database is a build error rather than a surprise
//at the bottom of a results table.
template <class D>
struct LogDbContract {
    static_assert(logdb_detail::has_batch_size<D>::value,
        "log db is missing: size_t batch_size() const");

    static_assert(logdb_detail::has_append<D>::value,
        "log db is missing: void append(const store::LogRecord&)");

    static_assert(logdb_detail::has_flush<D>::value,
        "log db is missing: void flush()");

    static_assert(logdb_detail::has_seek<D>::value,
        "log db is missing: Cursor seek(int64_t) const");

    static_assert(logdb_detail::has_next<D>::value,
        "log db is missing: bool next(Cursor&, store::LogRecord&) const");

    static_assert(logdb_detail::has_scan<D>::value,
        "log db is missing: void scan(int64_t, int64_t, "
        "std::vector<store::LogRecord>&) const");

    static_assert(logdb_detail::has_summarize<D>::value,
        "log db is missing: store::BatchSummary summarize(int64_t, int64_t) const");

    static_assert(logdb_detail::has_maintain<D>::value,
        "log db is missing: void maintain()");

    static_assert(logdb_detail::has_counters<D>::value,
        "log db is missing one of: int64_t records_scanned() const, "
        "int64_t records_returned() const, int64_t nodes_written() const");
};

} //namespace logdb

#endif
