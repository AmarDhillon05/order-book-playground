#include "bench/metrics.h"

#include <chrono>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

using namespace std;

namespace bench {

uint64_t read_tsc() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int aux;
    return __rdtscp(&aux);
#else
    //Everywhere else, fall back to the monotonic clock and accept that it costs more than
    //it measures on the cheapest books. The calibration below reports how much.
    return static_cast<uint64_t>(
        chrono::duration_cast<chrono::nanoseconds>(
            chrono::steady_clock::now().time_since_epoch()).count());
#endif
}


double tsc_ticks_per_ns() {
    //Calibrated once. A function local static is initialised on first use and is thread
    //safe in C++11 onward, which is what makes this safe to call from the timed loop -
    //though the loop calls it once, before starting, for exactly that reason.
    static double ticks_per_ns = 0.0;

    if(ticks_per_ns > 0.0) {
        return ticks_per_ns;
    }

#if defined(__x86_64__) || defined(__i386__)
    //10 ms is long enough that the two clock reads are noise against it and short enough
    //not to be felt at startup
    const chrono::steady_clock::time_point w0 = chrono::steady_clock::now();
    const uint64_t t0 = read_tsc();

    chrono::steady_clock::time_point w1;

    do {
        w1 = chrono::steady_clock::now();
    } while(chrono::duration_cast<chrono::nanoseconds>(w1 - w0).count() < 10000000LL);

    const uint64_t t1 = read_tsc();

    const double wall_ns = static_cast<double>(
        chrono::duration_cast<chrono::nanoseconds>(w1 - w0).count());

    ticks_per_ns = (wall_ns > 0)
        ? static_cast<double>(t1 - t0) / wall_ns
        : 1.0;
#else
    //read_tsc() already returns nanoseconds on the fallback path
    ticks_per_ns = 1.0;
#endif

    if(ticks_per_ns <= 0.0) {
        ticks_per_ns = 1.0;
    }

    return ticks_per_ns;
}


double calibrate_instrument_ns() {
    const double per_ns = tsc_ticks_per_ns();
    const int rounds = 100000;

    //Warm, so the first few iterations do not carry the cost of faulting the loop in
    for(int i = 0; i < 1000; i++) {
        volatile uint64_t t = read_tsc();
        (void)t;
    }

    //Written through so the optimizer cannot delete reads it can see nothing consumes
    volatile uint64_t sink = 0;

    const uint64_t start = read_tsc();

    for(int i = 0; i < rounds; i++) {
        //A pair, because that is what bracketing one operation costs
        const uint64_t a = read_tsc();
        const uint64_t b = read_tsc();

        sink += b - a;
    }

    const uint64_t end = read_tsc();

    (void)sink;

    return (static_cast<double>(end - start) / per_ns) / static_cast<double>(rounds);
}


//Values below this land in their own bucket, one per nanosecond. Sub-32 ns is where the
//control book and the instrument's own cost both live, and a log scale there would put
//them all in the same bucket.
const uint64_t HIST_LINEAR_LIMIT = 32;

//Sub buckets inside each power of two, as a shift
const int HIST_SUB_BITS = 4;
const uint64_t HIST_SUB = 1u << HIST_SUB_BITS;


//Value to bucket. Linear below the limit, then powers of two split 16 ways, which keeps
//the relative error under ~6% everywhere above it.
static size_t bucket_of(uint64_t v) {
    if(v < HIST_LINEAR_LIMIT) {
        return static_cast<size_t>(v);
    }

    const int msb = 63 - __builtin_clzll(v);
    const int shift = msb - HIST_SUB_BITS;
    const uint64_t sub = (v >> shift) & (HIST_SUB - 1);

    const size_t idx = static_cast<size_t>(msb - HIST_SUB_BITS) * HIST_SUB
                     + HIST_LINEAR_LIMIT + static_cast<size_t>(sub);

    return (idx < LATENCY_BUCKETS) ? idx : LATENCY_BUCKETS - 1;
}


//Bucket back to the smallest value it can hold. Deliberately the lower bound: a reported
//percentile is then a figure the run actually reached rather than one rounded past it.
static uint64_t value_of(size_t idx) {
    if(idx < HIST_LINEAR_LIMIT) {
        return static_cast<uint64_t>(idx);
    }

    const size_t off = idx - HIST_LINEAR_LIMIT;
    const int msb = static_cast<int>(off / HIST_SUB) + HIST_SUB_BITS;
    const uint64_t sub = static_cast<uint64_t>(off % HIST_SUB);

    return (HIST_SUB + sub) << (msb - HIST_SUB_BITS);
}


LatencyHist::LatencyHist()
    : buckets_(LATENCY_BUCKETS, 0), count_(0), min_(0), max_(0), total_ns_(0) {}


void LatencyHist::add(uint64_t ns) {
    if(count_ == 0 || ns < min_) {
        min_ = ns;
    }

    if(ns > max_) {
        max_ = ns;
    }

    buckets_[bucket_of(ns)]++;
    count_++;
    total_ns_ += static_cast<double>(ns);
}


uint64_t LatencyHist::percentile(double p) const {
    if(count_ == 0) {
        return 0;
    }

    //Rank of the sample wanted, clamped so p100 is the last sample and not one past it
    long long want = static_cast<long long>(
        (p / 100.0) * static_cast<double>(count_) + 0.5);

    if(want < 1) {
        want = 1;
    }

    if(want > count_) {
        want = count_;
    }

    long long seen = 0;

    for(size_t i = 0; i < buckets_.size(); i++) {
        seen += static_cast<long long>(buckets_[i]);

        if(seen >= want) {
            return value_of(i);
        }
    }

    return max_;
}


double LatencyHist::mean() const {
    if(count_ == 0) {
        return 0.0;
    }

    return total_ns_ / static_cast<double>(count_);
}


void LatencyHist::clear() {
    for(size_t i = 0; i < buckets_.size(); i++) {
        buckets_[i] = 0;
    }

    count_ = 0;
    min_ = 0;
    max_ = 0;
    total_ns_ = 0;
}


const char* op_kind_name(OpKind k) {
    switch(k) {
        case OP_ADD:     return "add";
        case OP_DELETE:  return "delete";
        case OP_MODIFY:  return "modify";
        case OP_EXECUTE: return "execute";
        case OP_OTHER:   return "other";
        default:         return "?";
    }
}


OpKind op_kind_of(char kind) {
    switch(kind) {
        case 'a': return OP_ADD;
        case 'R': return OP_DELETE;
        case 'M': return OP_MODIFY;
        case 'L': return OP_EXECUTE;
        default:  return OP_OTHER;
    }
}


LogDbOps::LogDbOps()
    : index_probes(0), file_opens(0), node_hops(0), page_reads(0), page_writes(0),
      nodes_written(0), records_scanned(0), records_returned(0), summary_folds(0),
      cache_hits(0), cache_misses(0) {}


double LogDbOps::read_amplification() const {
    if(records_returned == 0) {
        return 0.0;
    }

    return static_cast<double>(records_scanned) / static_cast<double>(records_returned);
}


double LogDbOps::cache_hit_rate() const {
    const long long total = cache_hits + cache_misses;

    if(total == 0) {
        return 0.0;
    }

    return static_cast<double>(cache_hits) / static_cast<double>(total);
}


AdaptiveOps::AdaptiveOps()
    : batch_updates(0), rebalances(0), nodes_rewritten(0), transactions(0),
      batch_size_first(0), batch_size_last(0), batch_size_min(0), batch_size_max(0) {}


double AdaptiveOps::transactions_per_batch() const {
    if(batch_updates == 0) {
        return 0.0;
    }

    return static_cast<double>(transactions) / static_cast<double>(batch_updates);
}


LogDbMetrics::LogDbMetrics()
    : present(false), adaptive_present(false), records_appended(0), append_wall_ns(0),
      records_read(0), read_wall_ns(0), bytes_requested(0), bytes_read(0),
      bytes_written(0), store_bytes_on_disk(0) {}


double LogDbMetrics::append_per_sec() const {
    if(append_wall_ns <= 0) {
        return 0.0;
    }

    return static_cast<double>(records_appended) * 1e9 / append_wall_ns;
}


double LogDbMetrics::read_per_sec() const {
    if(read_wall_ns <= 0) {
        return 0.0;
    }

    return static_cast<double>(records_read) * 1e9 / read_wall_ns;
}


BookOps::BookOps()
    : level_probes(0), order_hops(0), comparisons(0), id_probes(0), node_updates(0),
      tombstones_skipped(0), page_loads(0), evictions(0), cache_hits(0),
      cache_misses(0) {}


double BookOps::cache_hit_rate() const {
    const long long total = cache_hits + cache_misses;

    if(total == 0) {
        return 0.0;
    }

    return static_cast<double>(cache_hits) / static_cast<double>(total);
}


BookMetrics::BookMetrics()
    : events(0), best_ns(0), worst_ns(0), mean_ns(0), ns_per_event(0), events_per_sec(0),
      checksum(0), vs_naive(0), latency_present(false), instrument_ns(0),
      ops_present(false), symbols(0),
      live_orders(-1), live_levels(-1), rss_delta_bytes(-1), peak_rss_bytes(-1),
      bytes_per_event(0), bytes_per_symbol(-1), cpu_ns(-1), cpu_util(0),
      invol_switches(-1) {}


HardwareMetrics::HardwareMetrics()
    : available(false), ipc(0), retiring_pct(0), bad_speculation_pct(0),
      frontend_bound_pct(0), backend_bound_pct(0), memory_bound_pct(0), core_bound_pct(0),
      cycles(-1), instructions(-1), cache_references(-1), cache_misses(-1),
      branch_misses(-1), dtlb_load_misses(-1), pinned_cpu(-1), turbo_disabled(false) {}


AccuracyMetrics::AccuracyMetrics()
    : present(false), conserved(false), submitted_shares(0), accounted_shares(0),
      graded(0), exact(0), wrong_orders(0), wrong_sizes(0), too_thin(0),
      shares_expected(0), shares_right(0), crossed_adds(0), crossed_modifies(0),
      price_agreement(0), full_agreement(0) {}


double AccuracyMetrics::exact_rate() const {
    if(graded == 0) {
        return 0.0;
    }

    return static_cast<double>(exact) / static_cast<double>(graded);
}


double AccuracyMetrics::share_rate() const {
    if(shares_expected == 0) {
        return 0.0;
    }

    return static_cast<double>(shares_right) / static_cast<double>(shares_expected);
}


AppMetrics::AppMetrics()
    : events(0), load_wall_ns(0), bench_wall_ns(0), drift_wall_ns(0), total_wall_ns(0),
      events_per_sec(0), peak_rss_bytes(-1), read_bytes(-1), write_bytes(-1),
      read_chars(-1) {}

} //namespace bench
