//What a run measured, in the four categories a result is read in.
//
//  AppMetrics       the whole program: how long it took end to end, how each book
//                   compares to the naive baseline, whether it stayed correct
//  LogDbMetrics     the log database on its own: per operation timing, operations
//                   performed against data actually wanted, throughput and latency
//  BookMetrics      the order book on its own: the same three, plus the counters that
//                   are specific to one structure
//  HardwareMetrics  what the CPU was doing, from perf. Collected separately and
//                   deliberately, because it needs a different run and elevated access
//
//TWO KINDS OF NUMBER, AND ONE OF THEM IS BETTER
//
//Everything here is either a COUNT or a TIME, and they are not equally trustworthy.
//
//Counts are exact. The same input produces the same count on any machine, in any build,
//at any clock speed - so "the B+ tree opened 12 nodes where the LSM opened 340" is a
//finding that survives being re-run by someone else on different hardware. Times jitter:
//this machine alone is a three microarchitecture hybrid with a 400 MHz to 4.8 GHz
//frequency range, and an unpinned run can vary by more than the effect being measured.
//
//So the counts are the result and the times explain the constants. Where the two
//disagree, the counts are right.
//
//WHY LATENCY IS SEPARATE FROM THROUGHPUT
//
//A per operation timing costs something to take, and what it costs is not small next to
//what it measures - the control book applies an event in ~6 ns and a timestamp read is a
//meaningful fraction of that. So latency is collected on its own pass, never on the pass
//that produced the throughput figure, and the instrument's own cost is calibrated and
//reported next to it rather than folded in silently.

#ifndef BENCH_METRICS_H
#define BENCH_METRICS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

//Buckets in the latency histogram. Powers of two with 16 linear sub buckets inside each,
//so resolution is ~6% of the value across nine orders of magnitude - fine enough for a
//percentile and small enough to stay in cache while it is being filled.
const size_t LATENCY_BUCKETS = 1024;


//A preallocated latency histogram.
//
//Fixed size and filled by an increment, so recording a sample allocates nothing and does
//no I/O - MR-2 holds on the measured path. Percentiles come out of it afterwards, which
//is MR-1's requirement that latency is reported as a distribution and never as a mean.
class LatencyHist {
public:
    LatencyHist();

    void add(uint64_t ns);

    //Reported as the LOWER bound of the bucket the percentile falls in, so a figure is
    //never rounded upward into a number the run did not achieve
    uint64_t percentile(double p) const;

    uint64_t min() const { return (count_ > 0) ? min_ : 0; }
    uint64_t max() const { return max_; }
    long long count() const { return count_; }

    //Kept as a running total rather than derived from the buckets, so it is exact rather
    //than the sum of bucket midpoints. Only ever printed beside the percentiles.
    double mean() const;

    void clear();

private:
    std::vector<uint64_t> buckets_;
    long long count_;
    uint64_t min_;
    uint64_t max_;
    double total_ns_;
};


//--- the instrument itself --------------------------------------------------------------
//
//Per operation timing needs a clock that costs less than what it measures. The control
//book applies an event in ~6 ns, and clock_gettime through the vDSO is ~20-25 ns - so
//timing with it would report mostly itself. rdtscp is ~7 ns and is what is used here.
//
//The counter is invariant on this class of part (constant_tsc, nonstop_tsc), so it does
//not change rate with frequency or stop in an idle state. It is still only a tick count,
//so it is calibrated against a real clock once at startup.

//Raw counter. Serialising, so it cannot be reordered around the work being measured -
//which a plain rdtsc can, making a short region read as zero.
uint64_t read_tsc();

//Ticks per nanosecond, calibrated once against CLOCK_MONOTONIC and cached.
double tsc_ticks_per_ns();

//What one timestamp PAIR costs, measured by taking a great many of them around nothing.
//Reported in the output next to the figures it perturbs rather than subtracted from them:
//subtracting an average from a distribution would distort the tail, which is the part of
//a latency distribution worth having.
double calibrate_instrument_ns();


//Message kinds broken out separately. The stream is 47% adds, 47% deletes, 4.2%
//modifies and 0.23% executes, so one blended ns/event hides that these are entirely
//different operations with entirely different costs.
enum OpKind {
    OP_ADD,
    OP_DELETE,
    OP_MODIFY,
    OP_EXECUTE,
    OP_OTHER,
    OP_KIND_COUNT
};

const char* op_kind_name(OpKind k);

//Feed message type character to a bucket
OpKind op_kind_of(char kind);


//--- category 2: the log database -----------------------------------------------------

//Exact operation counts for one database. Every field is a thing that was DONE, so the
//table reads as measured big-O rather than as a timing that happens to correlate with it.
struct LogDbOps {
    //Finding the node
    long long index_probes;         //Binary search steps in the file index
    long long file_opens;           //Descriptors opened, which multi-file management exists to bound
    long long node_hops;            //Skip list forwards followed, or tree levels descended

    //Moving the data
    long long page_reads;
    long long page_writes;
    long long nodes_written;

    //The ratio this whole layer is judged on: how much was looked at against how much was
    //actually wanted. This is the adaptive variant's control input.
    long long records_scanned;
    long long records_returned;

    //Headers folded WITHOUT opening the node. The B+ tree's entire advantage is nodes it
    //did not have to read, and no timing figure attributes that - it only shows up here.
    long long summary_folds;

    //Page cache, which the hash keyed structures live or die on
    long long cache_hits;
    long long cache_misses;

    LogDbOps();

    //Scanned over returned. 1.0 is perfect - every record read was a record wanted.
    double read_amplification() const;

    double cache_hit_rate() const;
};


//What the adaptive variant did that the fixed one did not.
struct AdaptiveOps {
    long long batch_updates;        //Times the batch size was changed
    long long rebalances;           //Maintenance passes that reshaped nodes
    long long nodes_rewritten;      //Cost of those passes

    long long transactions;         //Appends served across the run
    size_t batch_size_first;
    size_t batch_size_last;
    size_t batch_size_min;
    size_t batch_size_max;

    AdaptiveOps();

    //Transactions per batch - how much work each resize decision was amortised over. A
    //small number means the database is thrashing its own tuning.
    double transactions_per_batch() const;
};


struct LogDbMetrics {
    std::string name;
    bool present;                   //False until a database is registered

    LogDbOps ops;
    AdaptiveOps adaptive;
    bool adaptive_present;

    //Per operation latency, each on its own distribution
    LatencyHist append_ns;
    LatencyHist seek_ns;
    LatencyHist scan_ns;

    //Whole database throughput
    long long records_appended;
    double append_wall_ns;
    long long records_read;
    double read_wall_ns;

    //Bytes wanted against bytes moved, which is the other half of read amplification and
    //the only place write amplification can come from
    long long bytes_requested;
    long long bytes_read;
    long long bytes_written;
    long long store_bytes_on_disk;

    LogDbMetrics();

    double append_per_sec() const;
    double read_per_sec() const;
};


//--- category 3: the order book -------------------------------------------------------

struct BookOps {
    long long level_probes;         //Price levels visited
    long long order_hops;           //Orders walked inside a queue
    long long comparisons;          //Tree descent steps, or heap sift steps
    long long id_probes;            //Lookups in the order id index
    long long node_updates;         //Segment tree aggregate propagation to the root

    //Heap only, and the reason the leetcode variant is worth running. Lazy deletion on a
    //stream that is 47% cancels leaves tombstones at the root, and this counts how often
    //a query had to step over one to find a live order.
    long long tombstones_skipped;

    //Paging, for the books that evict cold instruments
    long long page_loads;
    long long evictions;
    long long cache_hits;
    long long cache_misses;

    BookOps();

    double cache_hit_rate() const;
};


struct BookMetrics {
    std::string name;
    std::string description;

    long long events;

    //From the uninstrumented pass, so these are the headline figures
    double best_ns;
    double worst_ns;
    double mean_ns;
    double ns_per_event;
    double events_per_sec;
    int64_t checksum;

    //Against the naive baseline. 1.00 is parity; above 1 is slower.
    double vs_naive;

    //From the instrumented pass, which is a different run - see the note at the top
    bool latency_present;
    LatencyHist by_kind[OP_KIND_COUNT];
    LatencyHist all_ops;

    //What one timestamp pair cost on this machine. Printed beside the distribution
    //because it is the floor of every figure in it: a book whose p50 sits at this value
    //was not measured, it was outrun by its own instrument.
    double instrument_ns;

    BookOps ops;
    bool ops_present;

    //State held at the end
    size_t symbols;
    long long live_orders;
    long long live_levels;

    long long rss_delta_bytes;
    long long peak_rss_bytes;
    double bytes_per_event;
    long long bytes_per_symbol;

    long long cpu_ns;
    double cpu_util;
    long long invol_switches;

    BookMetrics();
};


//--- category 4: the hardware ---------------------------------------------------------

//Filled from perf, which needs a separate run and access this machine does not grant by
//default. Never self instrumented: perf_event_open on the measured path would change the
//thing being measured, and the repo already made that call - see the README.
struct HardwareMetrics {
    bool available;
    std::string unavailable_reason;

    //What to run to fill this in, printed in the report so the figure is reproducible
    //from the report alone
    std::string command;

    double ipc;

    //Top down: where every issue slot the core had actually went. These are the
    //"opportunities wasted" figures - a slot that did not retire an instruction is a slot
    //the machine had and could not use.
    double retiring_pct;
    double bad_speculation_pct;
    double frontend_bound_pct;
    double backend_bound_pct;
    double memory_bound_pct;
    double core_bound_pct;

    long long cycles;
    long long instructions;
    long long cache_references;
    long long cache_misses;
    long long branch_misses;
    long long dtlb_load_misses;

    //Pinning and clock state, per MR-4. A figure taken without these recorded is not
    //reproducible on a hybrid part where core type alone moves IPC.
    std::string cpu_model;
    int pinned_cpu;
    std::string core_type;
    std::string governor;
    bool turbo_disabled;

    HardwareMetrics();
};


//--- category 1: the whole application ------------------------------------------------

//Correctness, which gates everything else. A performance number taken from a run that
//did not stay correct is not a performance number.
struct AccuracyMetrics {
    bool present;

    //Conservation of shares: submitted == traded + cancelled + resting. Makes no
    //reference to IEX at all, so a failure is a bug and never a disagreement.
    bool conserved;
    long long submitted_shares;
    long long accounted_shares;

    //Free running fill agreement, from the drift replay
    long long graded;
    long long exact;
    long long wrong_orders;
    long long wrong_sizes;
    long long too_thin;

    long long shares_expected;
    long long shares_right;

    //Divergence the exchange saw without a second book to compare against
    long long crossed_adds;
    long long crossed_modifies;

    //Top of book agreement at the close of the replay
    double price_agreement;
    double full_agreement;

    AccuracyMetrics();

    double exact_rate() const;
    double share_rate() const;
};


struct AppMetrics {
    std::string capture;
    long long events;

    //End to end, split so the replay is not credited with the decode
    double load_wall_ns;
    double bench_wall_ns;
    double drift_wall_ns;
    double total_wall_ns;

    double events_per_sec;          //Over the whole program, decode included

    std::string baseline;           //The book everything else is compared against

    long long peak_rss_bytes;
    long long read_bytes;
    long long write_bytes;
    long long read_chars;

    AccuracyMetrics accuracy;

    AppMetrics();
};


//Everything one run produced, which is what the report writer is handed.
struct RunReport {
    AppMetrics app;
    std::vector<BookMetrics> books;
    std::vector<LogDbMetrics> logdbs;
    HardwareMetrics hardware;

    std::string command_line;
    std::string started_at;
    std::string host;
};

} //namespace bench

#endif
