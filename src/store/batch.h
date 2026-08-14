//A batch: the unit every database stores, and the unit every database reads back.
//
//Records are accumulated into a batch until it is full, then the batch becomes one node
//in whatever database is underneath. Nothing below this file knows what a message is; it
//knows a header and a run of fixed width records.
//
//THE SUMMARY IS THE POINT
//
//A node whose header carries only "where am I" is a node you have to open to learn
//anything from. BatchSummary makes the header answer questions on its own - the price
//range the batch covered, how many shares moved through it, when it starts and ends - so
//a query over a time window can decide whether a node is worth reading before reading it,
//and a query for an aggregate can often answer without reading any at all.
//
//What makes that work is that summaries MERGE. Two adjacent summaries combine into the
//summary their two batches would have produced together, associatively, so:
//
//  the B+ tree can hold the merge of its children in an interior node, which is what
//  makes it behave like a segment tree over time rather than only an index into one
//
//  the LSM can answer a range aggregate by folding the headers it skips over instead of
//  opening them
//
//Merging is why the mean price is stored as a sum and a count rather than as a mean.
//Averaging two averages is only correct when they are weighted by their counts, and a
//structure that stores the average has thrown the weight away. Sum and count merge by
//addition and stay exact.
//
//BatchSummary is written to disk inside the header, so it is a fixed layout POD. It has a
//default constructor - which does not affect that, only trivial copying matters - and the
//empty summary is the merge identity: records == 0.

#ifndef STORE_BATCH_H
#define STORE_BATCH_H

#include "store/record.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace store {

//Bytes of header prepended to every node, before the first record.
//
//128 is two record slots exactly, which is the whole reason for the number: records stay
//64 byte aligned inside the page, and the i'th record sits at BATCH_HEADER_SIZE +
//i * RECORD_SIZE with no correction term.
const size_t BATCH_HEADER_SIZE = 128;

//Room inside the header for whatever the database wants to prepend. The LSM keeps skip
//list forward pointers here; the B+ tree keeps its child links. Kept as opaque bytes so
//that batch.cpp does not have to know which database is using it, and so adding a
//database does not change the header layout for the others.
const size_t BATCH_DB_BYTES = 40;


//Mergeable digest of a run of records. Every field is either an extreme, a sum, or a
//count, because those are the three things that merge without reference to what is
//between them.
struct BatchSummary {
    int64_t records;                //Also the emptiness test: 0 is the merge identity

    //Sequence is the ordering authority on this feed, not time - see the note on
    //non-monotonic timestamps in resources/MEMORY.md. Carried so a node's position in the
    //stream is recoverable even where its timestamps overlap its neighbour's.
    int64_t first_seq;
    int64_t last_seq;

    int64_t first_ts_ns;
    int64_t last_ts_ns;

    //Top and bottom of the batch. Meaningful only when priced_records > 0: a batch of
    //system and status messages carries no price at all, and a min of 0 there would read
    //as a real price of zero.
    int64_t min_price;
    int64_t max_price;

    //The two halves of a mean, kept apart so the merge stays exact. Prices are ~2e6 at
    //four decimals, so the sum has room for ~4.6e12 records before it can overflow -
    //five orders of magnitude past a full day.
    int64_t price_sum;
    int64_t priced_records;

    int64_t total_size;             //Shares named by every record that carried a size

    BatchSummary();

    //Fold one record in. Cheap enough to call on the append path.
    void add(const LogRecord& r);

    //Fold another summary in, as if its records had been appended to this one's. `other`
    //must cover the LATER span: merging is associative but the seq and ts bounds assume
    //this summary comes first.
    void merge(const BatchSummary& other);

    bool empty() const { return records == 0; }

    //Mean price over the records that carried one. Zero when none did, which is not a
    //price - check priced_records before reading it as one.
    double mean_price() const;
};

static_assert(std::is_trivially_copyable<BatchSummary>::value,
    "BatchSummary is written inside a page header and must stay trivially copyable");


//What sits at the front of every node.
struct BatchHeader {
    BatchSummary summary;

    //The next node in stream order, or 0 for the last. Both LSM variants need this - a
    //scan that has run off the end of one node must continue without going back through
    //the index to ask where it is.
    int64_t next;

    uint8_t db[BATCH_DB_BYTES];
};

static_assert(sizeof(BatchHeader) == BATCH_HEADER_SIZE,
    "BatchHeader must fill exactly two record slots, or record alignment inside a page "
    "and every position calculation built on it are wrong");


//How many records fit in a page of this size, after the header.
inline size_t records_per_page(size_t page_size) {
    return (page_size > BATCH_HEADER_SIZE)
        ? (page_size - BATCH_HEADER_SIZE) / RECORD_SIZE
        : 0;
}


//A batch under construction, or one just read back.
//
//It does not own a capacity. Batch size is the database's to decide and the adaptive LSM
//changes it while running, so the limit is passed in at reset() rather than baked in
//here - a batch built under one size stays valid after the database has moved to another.
class Batch {
public:
    Batch();

    //Empty the batch and set the size it will report itself full at. Reused rather than
    //reconstructed on every flush, so the record vector keeps its allocation.
    void reset(size_t capacity);

    //Append and fold into the summary. The caller checks full() first; appending past
    //capacity is allowed and simply grows the batch, because a database that has just
    //shrunk its batch size still has to be able to close out the node it was filling.
    void append(const LogRecord& r);

    bool full() const { return records_.size() >= capacity_; }
    bool empty() const { return records_.empty(); }
    size_t size() const { return records_.size(); }
    size_t capacity() const { return capacity_; }

    const BatchSummary& summary() const { return header_.summary; }
    BatchHeader& header() { return header_; }
    const BatchHeader& header() const { return header_; }

    const LogRecord& at(size_t i) const { return records_[i]; }
    const std::vector<LogRecord>& records() const { return records_; }

    //Write header and records into a page sized buffer. Everything past the last record
    //is zeroed, so a page is a function of its contents alone and two runs producing the
    //same batch produce the same bytes. Throws if the batch does not fit.
    void serialize(void* page, size_t page_size) const;

    //Read a page back. Trusts the header's record count only as far as the page can hold,
    //so a corrupt or truncated count cannot walk off the end of the buffer.
    void parse(const void* page, size_t page_size);

private:
    BatchHeader header_;
    std::vector<LogRecord> records_;
    size_t capacity_;
};

} //namespace store

#endif
