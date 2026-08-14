#include "store/file_index.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace std;

namespace store {

//Identifies the file and pins the layout. A store written by a build with a different
//FileSpan must be refused rather than read as though the fields still line up.
const uint32_t INDEX_MAGIC = 0x58444946;        //'FIDX'
const uint32_t INDEX_VERSION = 1;


FileIndex::FileIndex() {}


void FileIndex::append(const FileSpan& s) {
    if(!spans_.empty()) {
        const FileSpan& prev = spans_.back();

        //Compared on the END of the previous span against the END of this one, not on the
        //starts. Starts can overlap legitimately - that is the non-monotonic timestamp
        //case the header describes - but a file that finishes before its predecessor
        //finished is not a later file, and find_first()'s binary search assumes otherwise.
        if(s.summary.last_ts_ns < prev.summary.last_ts_ns) {
            throw runtime_error("FileIndex::append: file "
                                + to_string(s.file_no) + " ends at "
                                + to_string(s.summary.last_ts_ns) + ", before file "
                                + to_string(prev.file_no) + " ended at "
                                + to_string(prev.summary.last_ts_ns));
        }
    }

    spans_.push_back(s);
    total_.merge(s.summary);
}


int FileIndex::find_first(int64_t ts_ns) const {
    //First span whose end is at or after ts_ns. Ends are non-decreasing by the check in
    //append(), so this is a plain lower bound - and it is the earliest file that could
    //hold the time even when the spans around it overlap.
    size_t lo = 0;
    size_t hi = spans_.size();

    while(lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;

        if(spans_[mid].summary.last_ts_ns < ts_ns) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if(lo >= spans_.size()) {
        return -1;
    }

    return static_cast<int>(lo);
}


void FileIndex::save(const string& path) const {
    FILE* f = fopen(path.c_str(), "wb");

    if(!f) {
        throw runtime_error("FileIndex::save: cannot open " + path);
    }

    const uint32_t magic = INDEX_MAGIC;
    const uint32_t version = INDEX_VERSION;
    const uint64_t count = spans_.size();

    //Recorded so a build whose FileSpan has changed shape is caught on load rather than
    //reading the array at the wrong stride
    const uint64_t span_bytes = sizeof(FileSpan);

    bool ok = fwrite(&magic, sizeof(magic), 1, f) == 1
           && fwrite(&version, sizeof(version), 1, f) == 1
           && fwrite(&span_bytes, sizeof(span_bytes), 1, f) == 1
           && fwrite(&count, sizeof(count), 1, f) == 1;

    if(ok && count > 0) {
        ok = fwrite(&spans_[0], sizeof(FileSpan), spans_.size(), f) == spans_.size();
    }

    if(fclose(f) != 0) {
        ok = false;
    }

    if(!ok) {
        throw runtime_error("FileIndex::save: write failed for " + path);
    }
}


void FileIndex::load(const string& path) {
    spans_.clear();
    total_ = BatchSummary();

    FILE* f = fopen(path.c_str(), "rb");

    if(!f) {
        //A store that has never been closed has no index. That is the state of a new one,
        //not a failure, so it loads as empty.
        return;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t span_bytes = 0;
    uint64_t count = 0;

    const bool read_header =
        fread(&magic, sizeof(magic), 1, f) == 1
        && fread(&version, sizeof(version), 1, f) == 1
        && fread(&span_bytes, sizeof(span_bytes), 1, f) == 1
        && fread(&count, sizeof(count), 1, f) == 1;

    if(!read_header) {
        fclose(f);
        throw runtime_error("FileIndex::load: " + path + " is too short to hold a header");
    }

    if(magic != INDEX_MAGIC) {
        fclose(f);
        throw runtime_error("FileIndex::load: " + path + " is not an index file");
    }

    if(version != INDEX_VERSION || span_bytes != sizeof(FileSpan)) {
        fclose(f);
        throw runtime_error("FileIndex::load: " + path + " is version "
                            + to_string(version) + " with " + to_string(span_bytes)
                            + " byte spans, this build reads version "
                            + to_string(INDEX_VERSION) + " with "
                            + to_string(sizeof(FileSpan)) + " byte spans");
    }

    if(count > 0) {
        spans_.resize(static_cast<size_t>(count));

        if(fread(&spans_[0], sizeof(FileSpan), spans_.size(), f) != spans_.size()) {
            fclose(f);
            spans_.clear();
            throw runtime_error("FileIndex::load: " + path + " claims "
                                + to_string(count) + " spans and does not hold them");
        }
    }

    fclose(f);

    //Rebuilt from the spans rather than stored beside them, so the total cannot drift out
    //of step with what it is a total of
    for(size_t i = 0; i < spans_.size(); i++) {
        total_.merge(spans_[i].summary);
    }
}

} //namespace store
