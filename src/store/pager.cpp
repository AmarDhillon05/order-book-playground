#include "store/pager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace std;

namespace store {

//errno as text, appended to whatever failed. Kept in one place so every throw in this
//file reports the same way.
static string errno_text() {
    return string(": ") + strerror(errno);
}


Pager::Pager(const string& dir, const string& prefix, size_t page_size,
         int64_t pages_per_file)
    : dir_(dir), prefix_(prefix), page_size_(page_size),
      pages_per_file_(pages_per_file), allocated_(0), reads_(0), writes_(0) {

    if(page_size_ < 512 || (page_size_ & (page_size_ - 1)) != 0) {
        throw runtime_error("Pager: page size must be a power of two at least 512, got "
                            + to_string(page_size_));
    }

    if(pages_per_file_ <= 0) {
        throw runtime_error("Pager: pages per file must be positive");
    }

    //mkdir rather than a check, because the check is a race. EEXIST is the success case.
    if(mkdir(dir_.c_str(), 0775) != 0 && errno != EEXIST) {
        throw runtime_error("Pager: mkdir " + dir_ + errno_text());
    }

    recover();
}


Pager::~Pager() {
    for(size_t i = 0; i < fds_.size(); i++) {
        if(fds_[i] >= 0) {
            close(fds_[i]);
        }
    }
}


string Pager::path_for(int32_t file_no) const {
    char buf[32];
    snprintf(buf, sizeof(buf), "_%05d.dat", file_no);

    return dir_ + "/" + prefix_ + buf;
}


string Pager::path_of(PageId id) const {
    return path_for(file_of(id));
}


//Count the files that exist and measure the last one. Allocation is strictly append only,
//so the highest page in the highest file is the cursor - nothing else has to be recorded
//and there is no separate metadata file to fall out of step with the data.
void Pager::recover() {
    int32_t file_no = 0;
    struct stat st;

    while(stat(path_for(file_no).c_str(), &st) == 0) {
        file_no++;
    }

    if(file_no == 0) {
        allocated_ = 0;
        return;
    }

    const int32_t last = file_no - 1;

    if(stat(path_for(last).c_str(), &st) != 0) {
        throw runtime_error("Pager: stat " + path_for(last) + errno_text());
    }

    const int64_t pages_in_last =
        static_cast<int64_t>(st.st_size) / static_cast<int64_t>(page_size_);

    allocated_ = static_cast<int64_t>(last) * pages_per_file_ + pages_in_last;

    //A file whose size is not a whole number of pages was cut off mid write. The partial
    //page is dropped rather than repaired: it is the tail of the log, its header cannot
    //be trusted, and silently handing it back as a node would decode as records.
    if(static_cast<int64_t>(st.st_size) % static_cast<int64_t>(page_size_) != 0) {
        fprintf(stderr, "warning: %s ends mid page (%lld bytes, page %zu) - "
                        "the partial page is dropped\n",
                path_for(last).c_str(), static_cast<long long>(st.st_size), page_size_);
    }
}


int Pager::fd_for(int32_t file_no) const {
    if(static_cast<size_t>(file_no) >= fds_.size()) {
        fds_.resize(static_cast<size_t>(file_no) + 1, -1);
    }

    if(fds_[file_no] >= 0) {
        return fds_[file_no];
    }

    const string path = path_for(file_no);

    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0664);

    if(fd < 0) {
        throw runtime_error("Pager: open " + path + errno_text());
    }

    fds_[file_no] = fd;

    return fd;
}


PageId Pager::alloc() {
    const PageId id = allocated_ + 1;

    //Zero the page through the normal write path, which extends the file. See the note in
    //the header: an id handed out without the file growing to cover it is an id the next
    //run will hand out again.
    vector<uint8_t> zeros(page_size_, 0);
    write(id, &zeros[0]);

    allocated_ = id;

    return id;
}


void Pager::read(PageId id, void* buf) const {
    if(id <= 0 || id > allocated_) {
        throw runtime_error("Pager::read: page " + to_string(id)
                            + " outside the store (holds " + to_string(allocated_) + ")");
    }

    const int fd = fd_for(file_of(id));

    //pread, not seek plus read: one syscall, and no file position for two readers of the
    //same descriptor to fight over
    const ssize_t n = pread(fd, buf, page_size_, offset_of(id));

    if(n < 0) {
        throw runtime_error("Pager::read: page " + to_string(id) + errno_text());
    }

    if(static_cast<size_t>(n) != page_size_) {
        throw runtime_error("Pager::read: page " + to_string(id) + " short read, got "
                            + to_string(n) + " of " + to_string(page_size_));
    }

    reads_++;
}


void Pager::write(PageId id, const void* buf) {
    if(id <= 0) {
        throw runtime_error("Pager::write: page " + to_string(id) + " is not a page");
    }

    const int fd = fd_for(file_of(id));

    const ssize_t n = pwrite(fd, buf, page_size_, offset_of(id));

    if(n < 0) {
        throw runtime_error("Pager::write: page " + to_string(id) + errno_text());
    }

    if(static_cast<size_t>(n) != page_size_) {
        throw runtime_error("Pager::write: page " + to_string(id) + " short write, put "
                            + to_string(n) + " of " + to_string(page_size_));
    }

    writes_++;
}


void Pager::sync() {
    for(size_t i = 0; i < fds_.size(); i++) {
        if(fds_[i] >= 0 && fsync(fds_[i]) != 0) {
            throw runtime_error("Pager::sync: " + path_for(static_cast<int32_t>(i))
                                + errno_text());
        }
    }
}

} //namespace store
