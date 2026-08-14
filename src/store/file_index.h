//Which file holds a given moment.
//
//The store is spread over many files and a query arrives as a time range, so something
//has to turn one into the other. This is that something, and it is deliberately the
//smallest structure that does the job: a sorted array of file spans, binary searched.
//
//WHY NOT THE SKIP LIST
//
//A skip list earns its keep by inserting into the middle of a sorted sequence in O(log n).
//At the file level there is no middle: files are created in stream order, appended, and
//never reordered or inserted between. The workload is append and binary search, and for
//that a sorted array IS the skip list's fast path with none of the pointer chasing - one
//contiguous run the prefetcher can see through, against a pointer hop per level.
//
//The skip list belongs one level down, inside the LSM, where nodes genuinely are inserted
//and rebalanced. Using it here would be paying for a capability the layer does not use.
//
//SPANS OVERLAP, AND THAT IS NOT A BUG
//
//Exchange timestamps are not monotonic - they run backwards within a packet, and sequence
//is the ordering authority (resources/MEMORY.md). So the last record of one file can carry
//a later time than the first record of the next, and two adjacent spans can overlap by the
//width of one packet's reordering.
//
//That makes "the file containing time T" the wrong question, because more than one file
//can. find_first() therefore returns the EARLIEST file that could contain T, and the
//caller walks forward while first_ts_ns <= T. A search that stops at the first hit will
//quietly miss records at a boundary, which is the kind of loss that shows up much later as
//a book that is a few orders light.

#ifndef STORE_FILE_INDEX_H
#define STORE_FILE_INDEX_H

#include "store/batch.h"
#include "store/pager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace store {

//What one file covers.
struct FileSpan {
    int32_t file_no;
    int32_t pad;                    //Explicit, so the on-disk layout has no implicit hole

    PageId first_page;
    PageId last_page;

    //Merged over every node in the file, which is the same merge the nodes themselves
    //use. It costs nothing extra to maintain and it means a range aggregate that fully
    //contains a file can fold this in without opening the file at all.
    BatchSummary summary;
};

static_assert(std::is_trivially_copyable<FileSpan>::value,
    "FileSpan is written to the index file with memcpy");


class FileIndex {
public:
    FileIndex();

    //Record a file's coverage. Files arrive in stream order and the index depends on
    //that, so a span ending before its predecessor is rejected rather than sorted into
    //place - it means the caller is writing two streams into one store, and the resulting
    //index would answer confidently and wrongly.
    void append(const FileSpan& s);

    //Earliest file that could hold ts_ns, or -1 if the index is empty or every file ends
    //before it. Walk forward from the result while at(i).summary.first_ts_ns <= ts_ns -
    //see the overlap note at the top.
    int find_first(int64_t ts_ns) const;

    size_t size() const { return spans_.size(); }
    bool empty() const { return spans_.empty(); }
    const FileSpan& at(size_t i) const { return spans_[i]; }

    //Everything the index covers, folded. Answers "what does this store hold" without
    //touching a data file.
    const BatchSummary& total() const { return total_; }

    //The index is small - one span per file, so tens of entries for a day - and is read
    //in full at startup and held in memory. It is written whole rather than appended to,
    //because rewriting tens of entries is cheaper than the bookkeeping that would make
    //appending safe.
    void save(const std::string& path) const;

    //Missing file is not an error: an index that has never been written describes a store
    //that has never been closed, which is the state of a new one.
    void load(const std::string& path);

private:
    std::vector<FileSpan> spans_;
    BatchSummary total_;
};

} //namespace store

#endif
