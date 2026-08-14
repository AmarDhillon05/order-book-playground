//Fixed size pages spread across a set of files.
//
//Everything a database keeps on disk goes through here, and nothing above this file opens
//a file or computes an offset. A node is a page; a database asks for one by id and gets
//page_size bytes back.
//
//WHY MORE THAN ONE FILE
//
//A single file is simpler and is the wrong shape for this. It has to be extended,
//fsynced and eventually copied as one object, its size is the unit of everything, and a
//day's capture is gigabytes - so any operation on the store is an operation on all of it.
//Rolling over at a cap keeps each file bounded, lets a query touch only the files whose
//time span it overlaps, and makes retiring old data an unlink rather than a rewrite.
//
//PAGE IDS ARE ORDERED, AND 1 BASED
//
//A page id is a global page number counting from 1, not a packed (file, page) pair. Two
//consequences, both used above this file:
//
//  0 is never a valid page, so it works as the null link in a node header without
//  needing a separate flag beside it.
//
//  ids increase with allocation, and this store only ever appends, so an id ordering IS
//  a stream ordering. An index over pages can binary search on the id directly and a scan
//  can walk ids upward without consulting anything.
//
//The file a page lives in is arithmetic on the id, so nothing has to be recorded to find
//it again.

#ifndef STORE_PAGER_H
#define STORE_PAGER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace store {

//Global page number, counting from 1. Zero is the null page and is never allocated.
typedef int64_t PageId;

const PageId NULL_PAGE = 0;

//4 KB matches the page the kernel will fault in anyway, so a read of one page is one
//fault rather than a partial one. Databases that want bigger nodes pass their own.
const size_t DEFAULT_PAGE_SIZE = 4096;

//At 4 KB pages this caps a file at 1 GB, which is small enough to unlink or copy without
//it being an event and large enough that a day is a handful of files rather than
//thousands of them.
const int64_t DEFAULT_PAGES_PER_FILE = 262144;


class Pager {
public:
    //Opens an existing store or starts a new one. Existing files are scanned to recover
    //the allocation cursor, so reopening continues where the last run stopped rather than
    //overwriting from page 1.
    Pager(const std::string& dir, const std::string& prefix,
      size_t page_size = DEFAULT_PAGE_SIZE,
      int64_t pages_per_file = DEFAULT_PAGES_PER_FILE);

    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    //Reserve the next page and zero it on disk.
    //
    //The zeroing is not tidiness. Allocation has to extend the file for the recovery scan
    //above to see it: a page reserved but never written leaves a hole, the file is shorter
    //than the cursor thinks, and the next run hands out an id that is already in use.
    PageId alloc();

    //Read one whole page into buf, which must hold page_size bytes. Throws on a short
    //read or an id past the end - both mean the caller's idea of the store is wrong, and
    //returning a half filled buffer would let that propagate as decoded garbage.
    void read(PageId id, void* buf) const;

    //Write one whole page from buf, which must hold page_size bytes.
    void write(PageId id, const void* buf);

    //Push open files to the device. Not called per write: a log that fsyncs every node
    //measures the device rather than the structure, and durability across a crash is not
    //something this project claims.
    void sync();

    size_t page_size() const { return page_size_; }
    int64_t pages_per_file() const { return pages_per_file_; }

    //Pages handed out so far, so the next alloc() returns page_count() + 1
    int64_t page_count() const { return allocated_; }

    int32_t file_of(PageId id) const {
        return static_cast<int32_t>((id - 1) / pages_per_file_);
    }

    int64_t offset_of(PageId id) const {
        return ((id - 1) % pages_per_file_) * static_cast<int64_t>(page_size_);
    }

    //Path of the file holding a page, for reporting and for tests that inspect the store
    std::string path_of(PageId id) const;

    long long reads() const { return reads_; }
    long long writes() const { return writes_; }

private:
    //Descriptor for one file, opened on first use and kept. Marked mutable because read()
    //is const and a read of a page in a file this run has not touched still has to open
    //it - the alternative is opening every file up front, which is worse for a store of
    //hundreds.
    int fd_for(int32_t file_no) const;

    std::string path_for(int32_t file_no) const;

    //Walk the existing files to find where allocation stopped
    void recover();

    std::string dir_;
    std::string prefix_;
    size_t page_size_;
    int64_t pages_per_file_;

    int64_t allocated_;

    mutable std::vector<int> fds_;

    mutable long long reads_;
    long long writes_;
};

} //namespace store

#endif
