#include "store/batch.h"

#include <cstring>
#include <stdexcept>
#include <string>

using namespace std;

namespace store {

BatchSummary::BatchSummary()
    : records(0), first_seq(0), last_seq(0), first_ts_ns(0), last_ts_ns(0),
      min_price(0), max_price(0), price_sum(0), priced_records(0), total_size(0) {}


void BatchSummary::add(const LogRecord& r) {
    if(records == 0) {
        first_seq = r.seq;

        if(r.present & HAS_TS) {
            first_ts_ns = r.ts_ns;
        }
    }

    records++;
    last_seq = r.seq;

    if(r.present & HAS_TS) {
        //Not a max. Timestamps run backwards within a packet, so the last record's time
        //is the span's end even when an earlier record carried a later one - the span
        //describes where the node sits in the stream, and stream order is sequence order.
        last_ts_ns = r.ts_ns;

        //A node whose first record carried no timestamp still needs a lower bound, or the
        //index cannot place it
        if(first_ts_ns == 0) {
            first_ts_ns = r.ts_ns;
        }
    }

    if(r.present & HAS_PRICE) {
        if(priced_records == 0) {
            min_price = r.price;
            max_price = r.price;

        } else {
            if(r.price < min_price) {
                min_price = r.price;
            }

            if(r.price > max_price) {
                max_price = r.price;
            }
        }

        price_sum += r.price;
        priced_records++;
    }

    if(r.present & HAS_SIZE) {
        total_size += static_cast<int64_t>(r.size);
    }
}


void BatchSummary::merge(const BatchSummary& other) {
    if(other.empty()) {
        return;
    }

    if(empty()) {
        *this = other;
        return;
    }

    //`other` covers the later span, so it supplies the upper bounds and this summary
    //keeps the lower ones
    last_seq = other.last_seq;

    if(other.last_ts_ns != 0) {
        last_ts_ns = other.last_ts_ns;
    }

    if(first_ts_ns == 0) {
        first_ts_ns = other.first_ts_ns;
    }

    //Guarded on both sides: a summary with no priced records has min and max at 0, which
    //would drag a real minimum down to zero if folded in blind
    if(other.priced_records > 0) {
        if(priced_records == 0) {
            min_price = other.min_price;
            max_price = other.max_price;

        } else {
            if(other.min_price < min_price) {
                min_price = other.min_price;
            }

            if(other.max_price > max_price) {
                max_price = other.max_price;
            }
        }
    }

    records += other.records;
    price_sum += other.price_sum;
    priced_records += other.priced_records;
    total_size += other.total_size;
}


double BatchSummary::mean_price() const {
    if(priced_records == 0) {
        return 0.0;
    }

    return static_cast<double>(price_sum) / static_cast<double>(priced_records);
}


//Value initialised rather than memset. BatchHeader is trivially copyable but not trivial
//- BatchSummary has a constructor - so assigning a fresh one is both what the compiler
//wants to see and the only form that stays correct if a member ever needs real
//construction. It zeroes the db prefix bytes and the next link too.
Batch::Batch() : header_(BatchHeader()), capacity_(0) {}


void Batch::reset(size_t capacity) {
    header_ = BatchHeader();

    //clear(), not a fresh vector: the batch is reused across every flush and the
    //allocation is worth keeping. reserve() here rather than on first append so the
    //append path never grows.
    records_.clear();
    records_.reserve(capacity);

    capacity_ = capacity;
}


void Batch::append(const LogRecord& r) {
    records_.push_back(r);
    header_.summary.add(r);
}


void Batch::serialize(void* page, size_t page_size) const {
    const size_t room = records_per_page(page_size);

    if(records_.size() > room) {
        throw runtime_error("Batch::serialize: " + to_string(records_.size())
                            + " records will not fit in a " + to_string(page_size)
                            + " byte page (room for " + to_string(room) + ")");
    }

    uint8_t* p = static_cast<uint8_t*>(page);

    //Zero first, so the tail past the last record is defined. Without it a half full node
    //carries whatever the buffer held before, two identical batches write different
    //bytes, and any page level checksum becomes meaningless.
    memset(p, 0, page_size);

    memcpy(p, &header_, sizeof(header_));

    if(!records_.empty()) {
        memcpy(p + BATCH_HEADER_SIZE, &records_[0],
               records_.size() * RECORD_SIZE);
    }
}


void Batch::parse(const void* page, size_t page_size) {
    if(page_size < BATCH_HEADER_SIZE) {
        throw runtime_error("Batch::parse: page of " + to_string(page_size)
                            + " bytes cannot hold a header");
    }

    const uint8_t* p = static_cast<const uint8_t*>(page);

    memcpy(&header_, p, sizeof(header_));

    //The count is data read off a file, so it is clamped rather than trusted. A corrupt
    //or truncated header must not be able to walk the read off the end of the buffer.
    size_t n = static_cast<size_t>(header_.summary.records);
    const size_t room = records_per_page(page_size);

    if(n > room) {
        n = room;
    }

    records_.resize(n);

    if(n > 0) {
        memcpy(&records_[0], p + BATCH_HEADER_SIZE, n * RECORD_SIZE);
    }

    //A node read back off disk is closed, not being filled, so it reports itself full.
    //Anything appending to it is resuming a node the writer left open and sets its own
    //capacity through reset() first.
    capacity_ = n;
}

} //namespace store
