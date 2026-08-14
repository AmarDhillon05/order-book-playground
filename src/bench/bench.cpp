//Comparative benchmark across the books in src/books.
//
//  ./bookbench <capture.pcap> [--books=a,b] [--events=N] [--reps=N] [--list]
//
//Adding a book is one line in REGISTRY below and nothing else. The benchmark does not
//know what a backend is made of, only that it answers to the contract in books/book.h.
//run_book() checks that contract at compile time, so a book that does not meet it fails
//the build rather than the results table.

#include "bench.h"
#include "bench/drift.h"
#include "bench/metrics.h"
#include "bench/report.h"
#include "books/null_book.h"
#include "books/naive_book.h"
#include "books/exchange_book.h"

#include <ctime>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

using namespace std;


//Every iteration under test. run is instantiated per backend, so the replay loop is
//compiled against the concrete type with nothing virtual between it and the storage.
//
//run_latency is a SECOND entry point over the same book, not a variant of the first: it
//brackets every apply() with a timestamp pair, which is too expensive to sit in the
//throughput path. Registering both here keeps the two runs compiled against the same
//concrete type, so the only difference between them is the instrument.
struct Entry {
    const char* name;
    const char* desc;
    BenchResult (*run)(const string&, const vector<Event>&, int);
    LatencyResult (*run_latency)(const vector<Event>&);
};

static const Entry REGISTRY[] = {
    { "null",  "control, stores nothing",
      &run_book<books::NullBook>, &run_book_latency<books::NullBook> },

    { "naive", "std::map levels + std::list time queue",
      &run_book<books::NaiveBook>, &run_book_latency<books::NaiveBook> },

    //Timed on ingest like any other book, which is a fair comparison and a slightly
    //different measurement: this one matches on arrival, so its apply() does strictly more
    //work than the mirror's. Note that the timed run gets NO reconstructed takers - the
    //benchmark loop only calls apply() - so it measures the cost of matching feed adds,
    //not of the full order flow. --drift is where the whole flow runs.
    { "exchange", "authoritative matching, decides its own fills",
      &run_book<books::ExchangeBook>, &run_book_latency<books::ExchangeBook> },
};

static const size_t REGISTRY_LEN = sizeof(REGISTRY) / sizeof(REGISTRY[0]);


//Option summary, printed on --help and on any argument the parser cannot make sense of
static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s <capture.pcap> [options]\n"
            "\n"
            "  --books=a,b   which books to run (default: all)\n"
            "  --events=N    replay at most N events (default: whole capture)\n"
            "  --reps=N      repetitions per book, best is reported (default: 3)\n"
            "  --list        list registered books and exit\n"
            "\n"
            "  --drift          also measure the exchange book against the mirror\n"
            "  --drift-sample=N take an agreement sample every N events\n"
            "                   (default 1000000; 0 for the closing sample only)\n"
            "\n"
            "  --latency        extra pass timing every apply() individually, reported\n"
            "                   as p50/p90/p99/p99.9/max per message kind\n"
            "  --report[=DIR]   file a full report under DIR (default logs/), numbered\n"
            "                   one past the highest already there\n"
            "\n"
            "--drift runs a second, untimed replay holding BOTH books at once and\n"
            "reconstructing takers up front, so it costs roughly double the memory and\n"
            "one extra pass. Use --events=N while iterating.\n"
            "\n"
            "--latency is a THIRD pass and its timings are not comparable to the\n"
            "throughput table: bracketing every call costs a timestamp pair per event,\n"
            "which is most of what the control book costs in the first place. The shape\n"
            "of the distribution is what it is for.\n",
            argv0);
}


//Walk REGISTRY and print one row per registered book, for --list and for name errors
static void list_books() {
    printf("registered books:\n");

    for(size_t i = 0; i < REGISTRY_LEN; i++) {
        printf("  %-8s  %s\n", REGISTRY[i].name, REGISTRY[i].desc);
    }
}


//Split a comma separated --books value
static vector<string> split_csv(const string& s) {
    vector<string> out;
    size_t start = 0;

    while(start <= s.size()) {
        size_t comma = s.find(',', start);
        size_t end = (comma == string::npos) ? s.size() : comma;

        if(end > start) {
            out.push_back(s.substr(start, end - start));
        }

        if(comma == string::npos) {
            break;
        }

        start = comma + 1;
    }

    return out;
}


//One row per book. ns/event and events/sec are derived from the best repetition, not the
//mean, so a run disturbed by the scheduler does not drag the number it is compared on.
static void print_results(const vector<BenchResult>& results) {
    printf("\n%-8s %12s %12s %10s %12s %14s\n",
           "book", "events", "best ms", "ns/event", "events/sec", "checksum");
    printf("%-8s %12s %12s %10s %12s %14s\n",
           "--------", "------------", "------------", "----------",
           "------------", "--------------");

    for(size_t i = 0; i < results.size(); i++) {
        const BenchResult& r = results[i];

        double per_event = (r.events > 0) ? r.best_ns / r.events : 0.0;
        double per_sec = (r.best_ns > 0) ? r.events / (r.best_ns / 1e9) : 0.0;

        printf("%-8s %12lld %12.3f %10.1f %12.0f %14lld\n",
               r.name.c_str(), r.events, r.best_ns / 1e6, per_event, per_sec,
               (long long)r.checksum);
    }

    printf("\n");
}


//Resource use, one row per book, kept as a second table because bolting eight more columns
//onto the timing table makes both unreadable.
//
//  book RSS       what the book itself added, on rep 0 only. Later reps have no clean
//                 baseline: freed pages are not reliably returned to the kernel.
//  b/symbol       the multi-book metric - cost of holding one instrument. spec.md's target
//                 is under 1 KB for an idle book, and this is where that gets reported.
//  cpu util       cpu time over wall time for the best repetition. The replay is single
//                 threaded, so ~100% is expected and a DIP is the finding, not a peak.
//  min flt        minor page faults: first touch of newly allocated pages. Tracks how much
//                 memory the run reached for, and is the usual reason cpu util sags.
//  invol cs       involuntary context switches. Nonzero means the scheduler preempted the
//                 measured region, which is the thing MR-5's repetitions exist to survive.
//  disk rd        bytes fetched from the block device during the region. The capture is read
//                 up front, so this should be ~0 - anything large means the page cache is
//                 missing and the numbers above are measuring I/O, not the book.
static void print_resources(const vector<BenchResult>& results) {
    printf("resource use\n");
    printf("%-8s %10s %10s %10s %8s %9s %10s %8s %10s\n",
           "book", "book RSS", "peak RSS", "b/event", "symbols", "b/symbol",
           "cpu util", "invol cs", "disk rd");
    printf("%-8s %10s %10s %10s %8s %9s %10s %8s %10s\n",
           "--------", "----------", "----------", "----------", "--------",
           "---------", "----------", "--------", "----------");

    for(size_t i = 0; i < results.size(); i++) {
        const BenchResult& r = results[i];

        //24, not 16: a long long is up to 20 digits plus a sign and a terminator
        char util[24];
        char per_sym[24];
        char invol[24];

        if(r.cpu_util > 0) {
            snprintf(util, sizeof(util), "%.1f%%", 100.0 * r.cpu_util);
        } else {
            snprintf(util, sizeof(util), "n/a");
        }

        if(r.bytes_per_symbol >= 0) {
            snprintf(per_sym, sizeof(per_sym), "%lld", r.bytes_per_symbol);
        } else {
            snprintf(per_sym, sizeof(per_sym), "-");
        }

        if(r.invol_switches >= 0) {
            snprintf(invol, sizeof(invol), "%lld", r.invol_switches);
        } else {
            snprintf(invol, sizeof(invol), "n/a");
        }

        printf("%-8s %10s %10s %10.1f %8zu %9s %10s %8s %10s\n",
               r.name.c_str(),
               format_bytes(r.rss_delta_bytes).c_str(),
               format_bytes(r.peak_rss_bytes).c_str(),
               r.bytes_per_event,
               r.symbols,
               per_sym,
               util,
               invol,
               format_bytes(r.read_bytes).c_str());
    }

    //Stated in the output and not only in the docs, because 39684 bytes per symbol read as
    //a per-book cost is wrong by more than an order of magnitude and someone will quote it.
    printf("\n  book RSS INCLUDES THE RETAINED EVENT LOG. run_book() default constructs\n"
           "  each book, and retention defaults on, so a book holds a second copy of the\n"
           "  stream - at 128 bytes an event that dominates the structure itself. b/symbol\n"
           "  is therefore NOT spec.md's memory-per-book metric yet; reaching that needs a\n"
           "  book constructed with DROP_LOG, which the generic harness cannot pass.\n");
    printf("  b/event and b/symbol are rep 0 only - later reps have no clean baseline,\n"
           "  since freed pages are not reliably returned to the kernel.\n\n");
}


//Disk, reported once for the run rather than per book: the input is shared and nothing here
//writes yet. The read amplification line is the one that will matter once a store exists -
//bytes actually pulled off the device against bytes the code asked for.
static void print_disk(const string& capture_path, const ResourceSnapshot& total) {
    DiskSpace d = snapshot_disk(capture_path);
    long long cap = file_size(capture_path);

    printf("disk\n");
    printf("  %-28s %14s\n", "capture on disk", format_bytes(cap).c_str());
    printf("  %-28s %14s\n", "read from device, whole run",
           format_bytes(total.read_bytes).c_str());
    printf("  %-28s %14s\n", "  requested via syscalls",
           format_bytes(total.read_chars).c_str());
    printf("  %-28s %14s\n", "written to device",
           format_bytes(total.write_bytes).c_str());
    printf("  %-28s %14s\n", "  requested via syscalls",
           format_bytes(total.write_chars).c_str());
    printf("  %-28s %14s\n", "volume free (available)",
           format_bytes(d.avail_bytes).c_str());
    printf("  %-28s %14s\n", "volume total",
           format_bytes(d.total_bytes).c_str());

    //Read amplification only means something once both halves are known and the code
    //actually asked for something
    if(total.read_bytes >= 0 && total.read_chars > 0) {
        printf("\n  %-28s %13.2fx\n", "read amplification",
               static_cast<double>(total.read_bytes)
                   / static_cast<double>(total.read_chars));
        printf("  a figure well under 1.00x means the page cache served the capture rather\n"
               "  than the disk, so the timing figures are measuring the book\n");
    }

    printf("\n");
}


//Percentage helper that reports a zero denominator as zero rather than as a NaN in a table
static double pct(long long n, long long d) {
    return (d > 0) ? 100.0 * static_cast<double>(n) / static_cast<double>(d) : 0.0;
}


//The drift report.
//
//Read top to bottom it answers three separate questions, and the order matters:
//
//  1. is the engine sound?      conservation. A gate, with no reference to IEX.
//  2. how far did it drift?     agreement against the mirror, over time.
//  3. why?                      crossed adds and orphans name the mechanism.
//
//The fill agreement figure here is deliberately NOT comparable to matchcheck's. matchcheck
//resyncs on truth after every taker so each comparison is independent; nothing resyncs
//here, so one wrong fill contaminates everything after it. A number below matchcheck's is
//the expected result, not a regression, and the gap between the two IS the drift.
static void print_drift(const DriftReport& d) {
    printf("\ndrift: exchange book vs mirror (naive)\n");
    printf("======================================\n");

    //First, because everything below is meaningless if this fails. A share that appeared
    //or vanished is a bug in ExchangeBook, not a disagreement with IEX.
    printf("\nconservation of shares %s\n", d.conserved ? "HOLDS" : "*** VIOLATED ***");
    printf("  %-28s %14lld\n", "submitted", d.submitted_shares);
    printf("  %-28s %14lld\n", "traded + cancelled + resting", d.accounted_shares);

    if(!d.conserved) {
        printf("  %-28s %14lld   <-- shares created or destroyed\n", "difference",
               d.accounted_shares - d.submitted_shares);
    }

    printf("\norder flow\n");
    printf("  %-28s %14lld\n", "takers reconstructed", d.takers);
    printf("  %-28s %14lld\n", "submitted to the exchange", d.submitted);
    printf("  %-28s %14lld\n", "skipped, side unresolvable", d.skipped_no_side);
    printf("  %-28s %14lld\n", "iex executions dropped", d.executes_dropped);

    printf("\nfill agreement, free running (no resync)\n");
    printf("  %-28s %14lld\n", "graded", d.graded);
    printf("  %-28s %14lld %7.2f%%\n", "  exact", d.exact, pct(d.exact, d.graded));
    printf("  %-28s %14lld %7.2f%%\n", "  wrong orders", d.wrong_orders,
           pct(d.wrong_orders, d.graded));
    printf("  %-28s %14lld %7.2f%%\n", "  wrong sizes", d.wrong_sizes,
           pct(d.wrong_sizes, d.graded));
    printf("  %-28s %14lld %7.2f%%\n", "  book too thin", d.too_thin,
           pct(d.too_thin, d.graded));
    printf("  %-28s %14lld\n", "ungraded, hidden liquidity", d.ungraded_hidden);
    printf("  %-28s %14lld %7.2f%%\n", "shares placed right", d.shares_right,
           pct(d.shares_right, d.shares_expected));

    printf("\ndivergence the exchange saw by itself\n");
    printf("  %-28s %14lld  (%lld shares)\n", "adds that crossed",
           d.crossed_adds, d.crossed_add_shares);
    printf("  %-28s %14lld  (%lld shares)\n", "modifies that crossed",
           d.crossed_modifies, d.crossed_modify_shares);
    printf("  %-28s %14lld\n", "orphans, mirror (floor)", d.mirror_orphans);
    printf("  %-28s %14lld\n", "orphans, exchange", d.exchange_orphans);
    printf("  %-28s %14lld\n", "  excess over floor",
           d.exchange_orphans - d.mirror_orphans);

    //The curve, not just the endpoint. A single closing figure cannot distinguish a book
    //that drifted immediately and then held from one that decayed steadily, and those are
    //different findings with different causes.
    printf("\ntop-of-book agreement over the replay\n");
    printf("  %14s %8s %9s %9s %12s %12s\n",
           "events", "symbols", "price", "px+size", "orders m/e", "shares m/e");
    printf("  %14s %8s %9s %9s %12s %12s\n",
           "--------------", "--------", "---------", "---------",
           "------------", "------------");

    for(size_t i = 0; i < d.samples.size(); i++) {
        const DriftSample& s = d.samples[i];

        printf("  %14lld %8zu %8.2f%% %8.2f%% %5lld/%-6lld %5lld/%-6lld\n",
               s.event_index + 1, s.symbols,
               pct(static_cast<long long>(s.price_agree),
                   static_cast<long long>(s.symbols)),
               pct(static_cast<long long>(s.full_agree),
                   static_cast<long long>(s.symbols)),
               s.mirror_orders, s.exchange_orders,
               s.mirror_shares, s.exchange_shares);
    }

    //Not a latency figure - this replay holds two books and scans both at intervals - but
    //holding two books is what sets the memory ceiling for the whole project, so the cost
    //is worth stating next to the result it bought.
    printf("\nwhat this measurement cost\n");
    printf("  %-28s %14.2f s\n", "wall", d.wall_ns / 1e9);

    if(d.cpu_ns >= 0) {
        printf("  %-28s %14.2f s\n", "cpu", d.cpu_ns / 1e9);
        printf("  %-28s %13.1f%%\n", "cpu util",
               (d.wall_ns > 0) ? 100.0 * static_cast<double>(d.cpu_ns)
                                     / static_cast<double>(d.wall_ns) : 0.0);
    }

    //Growth, not cost. If the timed benchmark ran first its books have already grown the
    //heap, and the allocator satisfies these two books out of those freed pages without
    //asking the kernel for more - so this legitimately reads 0 B while two full books are
    //live. Peak RSS is the figure that does not lie. Run with --books=null for a delta that
    //means what it looks like.
    printf("  %-28s %14s\n", "rss growth during replay",
           format_bytes(d.rss_delta_bytes).c_str());
    printf("  %-28s %14s   <-- the honest one\n", "peak rss",
           format_bytes(d.peak_rss_bytes).c_str());
    printf("  %-28s %14s\n", "read from device",
           format_bytes(d.read_bytes).c_str());

    if(d.minor_faults >= 0) {
        printf("  %-28s %14lld\n", "minor page faults", d.minor_faults);
    }

    if(d.events > 0) {
        printf("  %-28s %14.0f\n", "events/sec",
               (d.wall_ns > 0) ? static_cast<double>(d.events)
                                     / (d.wall_ns / 1e9) : 0.0);
    }

    printf("\n");
}


//Gather everything the run produced into the four categories and file it.
//
//Assembly is kept here rather than inside the report writer so that report.cpp stays a
//formatter with no opinion about where a number came from - which is what will let the
//log database and the new books add their own figures without touching it.
static string file_report(const string& path, const string& command_line,
                      const vector<Event>& events,
                      const vector<BenchResult>& results,
                      const vector<LatencyResult>& latencies,
                      const DriftReport& drift_rep, bool have_drift,
                      chrono::steady_clock::time_point prog_t0,
                      chrono::steady_clock::time_point load_t1,
                      chrono::steady_clock::time_point bench_t1,
                      chrono::steady_clock::time_point prog_t1,
                      const string& argv0, const string& dir) {
    bench::RunReport rep;

    rep.command_line = command_line;

    char when[64];
    const time_t now = time(0);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    rep.started_at = when;

    char host[256];

    if(gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = 0;
        rep.host = host;
    } else {
        rep.host = "unknown";
    }

    //--- category 1 -------------------------------------------------------------------
    bench::AppMetrics& a = rep.app;

    a.capture = path;
    a.events = static_cast<long long>(events.size());
    a.baseline = "naive";

    a.load_wall_ns = chrono::duration<double, nano>(load_t1 - prog_t0).count();
    a.bench_wall_ns = chrono::duration<double, nano>(bench_t1 - load_t1).count();
    a.drift_wall_ns = have_drift ? static_cast<double>(drift_rep.wall_ns) : 0.0;
    a.total_wall_ns = chrono::duration<double, nano>(prog_t1 - prog_t0).count();

    if(a.total_wall_ns > 0) {
        a.events_per_sec = static_cast<double>(a.events) * 1e9 / a.total_wall_ns;
    }

    const ResourceSnapshot final_snap = snapshot_resources();
    a.peak_rss_bytes = final_snap.peak_rss_bytes;
    a.read_bytes = final_snap.read_bytes;
    a.write_bytes = final_snap.write_bytes;
    a.read_chars = final_snap.read_chars;

    if(have_drift) {
        bench::AccuracyMetrics& acc = a.accuracy;

        acc.present = true;
        acc.conserved = drift_rep.conserved;
        acc.submitted_shares = drift_rep.submitted_shares;
        acc.accounted_shares = drift_rep.accounted_shares;
        acc.graded = drift_rep.graded;
        acc.exact = drift_rep.exact;
        acc.wrong_orders = drift_rep.wrong_orders;
        acc.wrong_sizes = drift_rep.wrong_sizes;
        acc.too_thin = drift_rep.too_thin;
        acc.shares_expected = drift_rep.shares_expected;
        acc.shares_right = drift_rep.shares_right;
        acc.crossed_adds = drift_rep.crossed_adds;
        acc.crossed_modifies = drift_rep.crossed_modifies;

        //The closing sample, which is the state the run finished in
        if(!drift_rep.samples.empty()) {
            const DriftSample& last = drift_rep.samples.back();

            if(last.symbols > 0) {
                acc.price_agreement = static_cast<double>(last.price_agree)
                                    / static_cast<double>(last.symbols);
                acc.full_agreement = static_cast<double>(last.full_agree)
                                   / static_cast<double>(last.symbols);
            }
        }
    }

    //--- category 3 -------------------------------------------------------------------
    //
    //Found before the loop so every row can be divided by it. Absent when the baseline
    //was not among the books run, in which case the column reports nothing rather than
    //comparing against whichever book happened to be first.
    double naive_ns = 0;

    for(size_t i = 0; i < results.size(); i++) {
        if(results[i].name == a.baseline) {
            naive_ns = results[i].best_ns;
            break;
        }
    }

    for(size_t i = 0; i < results.size(); i++) {
        const BenchResult& r = results[i];

        bench::BookMetrics b;

        b.name = r.name;
        b.events = r.events;
        b.best_ns = r.best_ns;
        b.worst_ns = r.worst_ns;
        b.mean_ns = r.mean_ns;
        b.checksum = r.checksum;

        if(r.events > 0) {
            b.ns_per_event = r.best_ns / static_cast<double>(r.events);
        }

        if(r.best_ns > 0) {
            b.events_per_sec = static_cast<double>(r.events) / (r.best_ns / 1e9);
        }

        b.vs_naive = (naive_ns > 0) ? r.best_ns / naive_ns : 0.0;

        b.symbols = r.symbols;
        b.rss_delta_bytes = r.rss_delta_bytes;
        b.peak_rss_bytes = r.peak_rss_bytes;
        b.bytes_per_event = r.bytes_per_event;
        b.bytes_per_symbol = r.bytes_per_symbol;
        b.cpu_ns = r.cpu_ns;
        b.cpu_util = r.cpu_util;
        b.invol_switches = r.invol_switches;

        if(i < latencies.size()) {
            b.latency_present = true;
            b.instrument_ns = latencies[i].instrument_ns;
            b.all_ops = latencies[i].all;

            for(int k = 0; k < bench::OP_KIND_COUNT; k++) {
                b.by_kind[k] = latencies[i].by_kind[k];
            }
        }

        //No book exposes operation counters yet. The report says so in full rather than
        //printing an empty table, because a table of zeroes reads as a measurement.
        b.ops_present = false;

        rep.books.push_back(b);
    }

    //--- category 2 -------------------------------------------------------------------
    //
    //Left empty deliberately. No backend implements logdb/logdb.h yet, and a section
    //populated with zeroes would be indistinguishable from one measuring a database that
    //did no work.

    //--- category 4 -------------------------------------------------------------------
    bench::probe_hardware(rep.hardware);

    string perf_args = path;

    if(!results.empty()) {
        perf_args += " --events=" + to_string(events.size());
    }

    rep.hardware.command =
        bench::perf_command(argv0, perf_args, rep.hardware.pinned_cpu);

    return bench::write_report(rep, dir);
}


//Parse the arguments, resolve the requested names against REGISTRY, decode the capture
//once, then hand that one event vector to each selected book in turn and print the table.
int main(int argc, char** argv) {
    string path;
    vector<string> wanted;
    long long limit = 0;
    int reps = 3;
    bool drift = false;
    long long drift_sample = 1000000;
    bool latency = false;
    bool report = false;
    string report_dir = "logs";

    //Kept verbatim for the report, so a filed result carries the command that produced it
    string command_line;

    for(int i = 0; i < argc; i++) {
        if(i > 0) {
            command_line += " ";
        }

        command_line += argv[i];
    }

    for(int i = 1; i < argc; i++) {
        string arg = argv[i];

        if(arg == "--list") {
            list_books();
            return 0;

        } else if(arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;

        } else if(arg.compare(0, 8, "--books=") == 0) {
            wanted = split_csv(arg.substr(8));

        } else if(arg.compare(0, 9, "--events=") == 0) {
            limit = atoll(arg.c_str() + 9);

        } else if(arg.compare(0, 7, "--reps=") == 0) {
            reps = atoi(arg.c_str() + 7);

        } else if(arg == "--drift") {
            drift = true;

        } else if(arg.compare(0, 15, "--drift-sample=") == 0) {
            drift_sample = atoll(arg.c_str() + 15);
            drift = true;

        } else if(arg == "--latency") {
            latency = true;

        } else if(arg == "--report") {
            report = true;

        } else if(arg.compare(0, 9, "--report=") == 0) {
            report = true;
            report_dir = arg.substr(9);

        } else if(!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;

        } else if(path.empty()) {
            path = arg;

        } else {
            fprintf(stderr, "unexpected argument: %s\n", arg.c_str());
            return 2;
        }
    }

    if(path.empty()) {
        usage(argv[0]);
        return 2;
    }

    if(reps < 1) {
        fprintf(stderr, "error: --reps must be at least 1\n");
        return 2;
    }

    //Resolve the requested names before doing any work, so a typo fails immediately
    //rather than after a long load
    vector<const Entry*> selected;

    if(wanted.empty()) {
        for(size_t i = 0; i < REGISTRY_LEN; i++) {
            selected.push_back(&REGISTRY[i]);
        }

    } else {
        for(size_t w = 0; w < wanted.size(); w++) {
            const Entry* found = 0;

            for(size_t i = 0; i < REGISTRY_LEN; i++) {
                if(wanted[w] == REGISTRY[i].name) {
                    found = &REGISTRY[i];
                    break;
                }
            }

            if(!found) {
                fprintf(stderr, "error: no book named '%s'\n\n", wanted[w].c_str());
                list_books();
                return 2;
            }

            selected.push_back(found);
        }
    }

    try {
        //Bracketed so the report can separate decode cost from replay cost. Decoding 51M
        //messages is a substantial fraction of a full run, and crediting it to the books
        //would make every one of them look worse by the same constant.
        const chrono::steady_clock::time_point prog_t0 = chrono::steady_clock::now();

        fprintf(stderr, "loading %s ...\n", path.c_str());

        vector<Event> events = load_events(path, limit);

        const chrono::steady_clock::time_point load_t1 = chrono::steady_clock::now();

        fprintf(stderr, "%zu events loaded, %d rep(s) each\n", events.size(), reps);

        if(events.empty()) {
            fprintf(stderr, "error: capture yielded no events\n");
            return 1;
        }

        vector<BenchResult> results;

        for(size_t i = 0; i < selected.size(); i++) {
            fprintf(stderr, "  running %s ...\n", selected[i]->name);
            results.push_back(selected[i]->run(selected[i]->name, events, reps));
        }

        const chrono::steady_clock::time_point bench_t1 = chrono::steady_clock::now();

        print_results(results);
        print_resources(results);
        print_disk(path, snapshot_resources());

        //A third pass, after the timed table so it cannot disturb it
        vector<LatencyResult> latencies;

        if(latency) {
            for(size_t i = 0; i < selected.size(); i++) {
                fprintf(stderr, "  timing %s per operation ...\n", selected[i]->name);
                latencies.push_back(selected[i]->run_latency(events));
            }
        }

        //After the timed table, and untimed. This replay holds both books at once and
        //calls get_all_tops() at intervals, neither of which belongs anywhere near a
        //latency figure.
        DriftReport drift_rep;
        bool have_drift = false;

        if(drift) {
            fprintf(stderr, "  measuring drift ...\n");
            drift_rep = measure_drift(events, drift_sample);
            have_drift = true;
            print_drift(drift_rep);
        }

        const chrono::steady_clock::time_point prog_t1 = chrono::steady_clock::now();

        if(report) {
            const string filed = file_report(path, command_line, events, results,
                                             latencies, drift_rep, have_drift,
                                             prog_t0, load_t1, bench_t1, prog_t1,
                                             argv[0], report_dir);

            printf("report filed: %s\n\n", filed.c_str());
        }

    } catch(const exception& err) {
        fprintf(stderr, "error: %s\n", err.what());
        return 1;
    }

    return 0;
}
