#include "bench/report.h"

#include <dirent.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace bench {

//Width of the rule under a section title, and of the title bars
const int RULE = 78;


static void rule(FILE* out, char c) {
    for(int i = 0; i < RULE; i++) {
        fputc(c, out);
    }

    fputc('\n', out);
}


static void section(FILE* out, const char* title) {
    fprintf(out, "\n--- %s ", title);

    const int used = 5 + static_cast<int>(strlen(title));

    for(int i = used; i < RULE; i++) {
        fputc('-', out);
    }

    fprintf(out, "\n\n");
}


//Bytes at three significant figures. Matches how the existing resource table reads, so
//two outputs sit next to each other without a unit change between them.
static string human_bytes(long long b) {
    if(b < 0) {
        return "-";
    }

    const char* unit[] = { "B", "K", "M", "G", "T" };
    double v = static_cast<double>(b);
    int u = 0;

    while(v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }

    char buf[32];

    if(u == 0) {
        snprintf(buf, sizeof(buf), "%lld B", b);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", v, unit[u]);
    }

    return string(buf);
}


static string human_ns(double ns) {
    char buf[32];

    if(ns < 0) {
        return "-";
    }

    if(ns < 1000.0) {
        snprintf(buf, sizeof(buf), "%.0f ns", ns);
    } else if(ns < 1e6) {
        snprintf(buf, sizeof(buf), "%.2f us", ns / 1e3);
    } else if(ns < 1e9) {
        snprintf(buf, sizeof(buf), "%.2f ms", ns / 1e6);
    } else {
        snprintf(buf, sizeof(buf), "%.3f s", ns / 1e9);
    }

    return string(buf);
}


//Read a whole small file, trimmed. Used for the /sys and /proc probes, all of which are
//one short line.
static string read_line_file(const char* path) {
    FILE* f = fopen(path, "r");

    if(!f) {
        return string();
    }

    char buf[512];
    const char* got = fgets(buf, sizeof(buf), f);

    fclose(f);

    if(!got) {
        return string();
    }

    string s(buf);

    while(!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == ' ')) {
        s.erase(s.size() - 1);
    }

    return s;
}


//Is `cpu` listed in a sysfs cpu list like "0-11" or "0-3,8,12-15"?
static bool cpu_in_list(const string& list, int cpu) {
    size_t i = 0;

    while(i < list.size()) {
        size_t comma = list.find(',', i);

        if(comma == string::npos) {
            comma = list.size();
        }

        const string part = list.substr(i, comma - i);
        const size_t dash = part.find('-');

        if(dash == string::npos) {
            if(atoi(part.c_str()) == cpu) {
                return true;
            }

        } else {
            const int lo = atoi(part.substr(0, dash).c_str());
            const int hi = atoi(part.substr(dash + 1).c_str());

            if(cpu >= lo && cpu <= hi) {
                return true;
            }
        }

        i = comma + 1;
    }

    return false;
}


void probe_hardware(HardwareMetrics& hw) {
    //Model, off the first "model name" line
    FILE* f = fopen("/proc/cpuinfo", "r");

    if(f) {
        char buf[512];

        while(fgets(buf, sizeof(buf), f)) {
            if(strncmp(buf, "model name", 10) == 0) {
                const char* colon = strchr(buf, ':');

                if(colon) {
                    string s(colon + 1);

                    while(!s.empty() && (s[0] == ' ' || s[0] == '\t')) {
                        s.erase(0, 1);
                    }

                    while(!s.empty() && s[s.size() - 1] == '\n') {
                        s.erase(s.size() - 1);
                    }

                    hw.cpu_model = s;
                }

                break;
            }
        }

        fclose(f);
    }

    //Which CPU this run is actually on. Recorded rather than assumed: on a hybrid part
    //the scheduler is free to move a single threaded benchmark onto an efficiency core,
    //and the same code there is a different measurement.
    hw.pinned_cpu = sched_getcpu();

    const string p_cores = read_line_file("/sys/devices/cpu_core/cpus");
    const string e_cores = read_line_file("/sys/devices/cpu_atom/cpus");

    if(p_cores.empty() && e_cores.empty()) {
        hw.core_type = "uniform";

    } else if(hw.pinned_cpu >= 0 && cpu_in_list(p_cores, hw.pinned_cpu)) {
        hw.core_type = "P-core";

    } else if(hw.pinned_cpu >= 0 && cpu_in_list(e_cores, hw.pinned_cpu)) {
        hw.core_type = "E-core";

    } else {
        hw.core_type = "unknown";
    }

    //Whether the affinity mask actually pins this process, as against it merely happening
    //to be on one CPU right now
    cpu_set_t set;
    CPU_ZERO(&set);

    bool pinned = false;

    if(sched_getaffinity(0, sizeof(set), &set) == 0) {
        pinned = (CPU_COUNT(&set) == 1);
    }

    if(!pinned) {
        hw.core_type += " (NOT pinned)";
    }

    hw.governor = read_line_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");

    if(hw.governor.empty()) {
        hw.governor = "unknown";
    }

    hw.turbo_disabled = (read_line_file("/sys/devices/system/cpu/intel_pstate/no_turbo")
                         == "1");

    //Counters are only readable below paranoid level 3. Checked rather than attempted,
    //so the report says WHY the section is empty instead of leaving it blank.
    const string paranoid =
        read_line_file("/proc/sys/kernel/perf_event_paranoid");

    if(paranoid.empty()) {
        hw.available = false;
        hw.unavailable_reason = "perf_event_paranoid could not be read";

    } else if(atoi(paranoid.c_str()) > 2) {
        hw.available = false;
        hw.unavailable_reason = "perf_event_paranoid is " + paranoid
            + "; counters need <= 2. Fix with: "
              "sudo sysctl -w kernel.perf_event_paranoid=1";

    } else {
        //Readable in principle. The figures still come from a separate perf run, which
        //this binary does not launch - see the note in metrics.h.
        hw.available = false;
        hw.unavailable_reason =
            "counters are readable, but perf is run separately - see the command below";
    }
}


string perf_command(const string& argv0, const string& args, int cpu) {
    char buf[1024];

    snprintf(buf, sizeof(buf),
             "taskset -c %d perf stat -r 5 -M TopdownL1 "
             "-e cycles,instructions,cache-references,cache-misses,"
             "branch-misses,dTLB-load-misses \\\n"
             "    %s %s",
             (cpu >= 0) ? cpu : 2, argv0.c_str(), args.c_str());

    return string(buf);
}


//--- the four sections ------------------------------------------------------------------

static void format_app(const RunReport& r, FILE* out) {
    section(out, "1. application");

    const AppMetrics& a = r.app;

    fprintf(out, "  capture                       %s\n", a.capture.c_str());
    fprintf(out, "  events replayed               %lld\n", a.events);
    fprintf(out, "  baseline for comparison       %s\n", a.baseline.c_str());
    fprintf(out, "\n");

    fprintf(out, "  timing\n");
    fprintf(out, "    decode the capture          %14s\n", human_ns(a.load_wall_ns).c_str());
    fprintf(out, "    benchmark replays           %14s\n", human_ns(a.bench_wall_ns).c_str());
    fprintf(out, "    drift replay                %14s\n", human_ns(a.drift_wall_ns).c_str());
    fprintf(out, "    whole program               %14s\n", human_ns(a.total_wall_ns).c_str());
    fprintf(out, "    end to end throughput       %14.0f events/sec\n", a.events_per_sec);
    fprintf(out, "\n");

    fprintf(out, "  resources\n");
    fprintf(out, "    peak RSS                    %14s\n",
            human_bytes(a.peak_rss_bytes).c_str());
    fprintf(out, "    read from device            %14s\n",
            human_bytes(a.read_bytes).c_str());
    fprintf(out, "    requested via syscalls      %14s\n",
            human_bytes(a.read_chars).c_str());
    fprintf(out, "    written to device           %14s\n",
            human_bytes(a.write_bytes).c_str());

    if(a.read_chars > 0 && a.read_bytes >= 0) {
        fprintf(out, "    read amplification          %14.2fx\n",
                static_cast<double>(a.read_bytes) / static_cast<double>(a.read_chars));
    }

    //Correctness first among equals: a timing from a run that did not stay correct is not
    //a timing, so this sits inside the application section rather than beside it.
    fprintf(out, "\n  accuracy against the feed\n");

    const AccuracyMetrics& acc = a.accuracy;

    if(!acc.present) {
        fprintf(out, "    not measured on this run (pass --drift)\n");
        return;
    }

    fprintf(out, "    conservation of shares      %14s\n",
            acc.conserved ? "HOLDS" : "*** VIOLATED ***");
    fprintf(out, "      submitted                 %14lld\n", acc.submitted_shares);
    fprintf(out, "      traded+cancelled+resting  %14lld\n", acc.accounted_shares);
    fprintf(out, "\n");
    fprintf(out, "    fills graded, free running  %14lld\n", acc.graded);
    fprintf(out, "      exact                     %14lld  %6.2f%%\n",
            acc.exact, acc.exact_rate() * 100.0);
    fprintf(out, "      wrong orders              %14lld\n", acc.wrong_orders);
    fprintf(out, "      wrong sizes               %14lld\n", acc.wrong_sizes);
    fprintf(out, "      book too thin             %14lld\n", acc.too_thin);
    fprintf(out, "    shares placed right         %14lld  %6.2f%%\n",
            acc.shares_right, acc.share_rate() * 100.0);
    fprintf(out, "\n");
    fprintf(out, "    adds that crossed           %14lld\n", acc.crossed_adds);
    fprintf(out, "    modifies that crossed       %14lld\n", acc.crossed_modifies);
    fprintf(out, "    top of book, price          %14.2f%%\n", acc.price_agreement * 100.0);
    fprintf(out, "    top of book, price+size     %14.2f%%\n", acc.full_agreement * 100.0);
}


static void format_latency_row(FILE* out, const char* label, const LatencyHist& h) {
    if(h.count() == 0) {
        return;
    }

    fprintf(out, "  %-9s %12lld %8llu %8llu %8llu %8llu %8llu %10.1f\n",
            label,
            h.count(),
            static_cast<unsigned long long>(h.percentile(50)),
            static_cast<unsigned long long>(h.percentile(90)),
            static_cast<unsigned long long>(h.percentile(99)),
            static_cast<unsigned long long>(h.percentile(99.9)),
            static_cast<unsigned long long>(h.max()),
            h.mean());
}


static void format_logdb(const RunReport& r, FILE* out) {
    section(out, "2. log database");

    if(r.logdbs.empty()) {
        fprintf(out,
                "  No log database registered on this run.\n"
                "\n"
                "  The substrate is in place - store/{record,batch,pager,file_index} and\n"
                "  the contract in logdb/logdb.h - but no backend implements it yet, so\n"
                "  there is nothing to measure. The five historical queries in\n"
                "  books/book.h are still answered by replaying a retained in-memory log,\n"
                "  which is the retention cost this layer exists to remove.\n");
        return;
    }

    for(size_t i = 0; i < r.logdbs.size(); i++) {
        const LogDbMetrics& d = r.logdbs[i];

        fprintf(out, "  %s\n", d.name.c_str());
        fprintf(out, "  %s\n\n", string(d.name.size(), '~').c_str());

        fprintf(out, "  throughput\n");
        fprintf(out, "    records appended          %14lld\n", d.records_appended);
        fprintf(out, "    append rate               %14.0f rec/sec\n", d.append_per_sec());
        fprintf(out, "    records read              %14lld\n", d.records_read);
        fprintf(out, "    read rate                 %14.0f rec/sec\n", d.read_per_sec());
        fprintf(out, "\n");

        fprintf(out, "  latency, ns per operation\n");
        fprintf(out, "  %-9s %12s %8s %8s %8s %8s %8s %10s\n",
                "op", "count", "p50", "p90", "p99", "p99.9", "max", "mean");
        format_latency_row(out, "append", d.append_ns);
        format_latency_row(out, "seek", d.seek_ns);
        format_latency_row(out, "scan", d.scan_ns);
        fprintf(out, "\n");

        //The exact half. These do not vary with the machine, the clock, or which core the
        //scheduler picked, which is why they and not the timings above are the result.
        fprintf(out, "  operations performed (exact, machine independent)\n");
        fprintf(out, "    index probes              %14lld\n", d.ops.index_probes);
        fprintf(out, "    file opens                %14lld\n", d.ops.file_opens);
        fprintf(out, "    node hops                 %14lld\n", d.ops.node_hops);
        fprintf(out, "    page reads                %14lld\n", d.ops.page_reads);
        fprintf(out, "    page writes               %14lld\n", d.ops.page_writes);
        fprintf(out, "    nodes written             %14lld\n", d.ops.nodes_written);
        fprintf(out, "    summary folds             %14lld\n", d.ops.summary_folds);
        fprintf(out, "\n");

        fprintf(out, "  work done against work wanted\n");
        fprintf(out, "    records scanned           %14lld\n", d.ops.records_scanned);
        fprintf(out, "    records returned          %14lld\n", d.ops.records_returned);
        fprintf(out, "    read amplification        %14.2fx\n",
                d.ops.read_amplification());
        fprintf(out, "    bytes requested           %14s\n",
                human_bytes(d.bytes_requested).c_str());
        fprintf(out, "    bytes read from device    %14s\n",
                human_bytes(d.bytes_read).c_str());
        fprintf(out, "    bytes written             %14s\n",
                human_bytes(d.bytes_written).c_str());
        fprintf(out, "    store on disk             %14s\n",
                human_bytes(d.store_bytes_on_disk).c_str());
        fprintf(out, "\n");

        fprintf(out, "  page cache\n");
        fprintf(out, "    hits                      %14lld\n", d.ops.cache_hits);
        fprintf(out, "    misses                    %14lld\n", d.ops.cache_misses);
        fprintf(out, "    hit rate                  %14.2f%%\n",
                d.ops.cache_hit_rate() * 100.0);

        if(d.adaptive_present) {
            fprintf(out, "\n  adaptive tuning\n");
            fprintf(out, "    batch size changes        %14lld\n",
                    d.adaptive.batch_updates);
            fprintf(out, "    rebalances                %14lld\n",
                    d.adaptive.rebalances);
            fprintf(out, "    nodes rewritten           %14lld\n",
                    d.adaptive.nodes_rewritten);
            fprintf(out, "    transactions              %14lld\n",
                    d.adaptive.transactions);
            fprintf(out, "    transactions per change   %14.1f\n",
                    d.adaptive.transactions_per_batch());
            fprintf(out, "    batch size first / last   %8zu / %zu\n",
                    d.adaptive.batch_size_first, d.adaptive.batch_size_last);
            fprintf(out, "    batch size min / max      %8zu / %zu\n",
                    d.adaptive.batch_size_min, d.adaptive.batch_size_max);
        }

        fprintf(out, "\n");
    }
}


static void format_books(const RunReport& r, FILE* out) {
    section(out, "3. order book");

    if(r.books.empty()) {
        fprintf(out, "  No book was run.\n");
        return;
    }

    fprintf(out, "  throughput\n");
    fprintf(out, "  %-10s %12s %12s %10s %12s %9s %16s\n",
            "book", "events", "best ms", "ns/event", "events/sec", "vs naive", "checksum");
    fprintf(out, "  %-10s %12s %12s %10s %12s %9s %16s\n",
            "--------", "------------", "------------", "----------",
            "------------", "---------", "----------------");

    for(size_t i = 0; i < r.books.size(); i++) {
        const BookMetrics& b = r.books[i];

        char vs[16];

        if(b.vs_naive > 0) {
            snprintf(vs, sizeof(vs), "%.2fx", b.vs_naive);
        } else {
            snprintf(vs, sizeof(vs), "%s", "-");
        }

        fprintf(out, "  %-10s %12lld %12.3f %10.1f %12.0f %9s %16lld\n",
                b.name.c_str(), b.events, b.best_ns / 1e6, b.ns_per_event,
                b.events_per_sec, vs, static_cast<long long>(b.checksum));
    }

    fprintf(out, "\n  memory and scheduling\n");
    fprintf(out, "  %-10s %12s %12s %10s %10s %9s %9s\n",
            "book", "book RSS", "peak RSS", "b/event", "symbols", "cpu util", "invol cs");
    fprintf(out, "  %-10s %12s %12s %10s %10s %9s %9s\n",
            "--------", "------------", "------------", "----------", "----------",
            "---------", "---------");

    for(size_t i = 0; i < r.books.size(); i++) {
        const BookMetrics& b = r.books[i];

        fprintf(out, "  %-10s %12s %12s %10.1f %10zu %8.1f%% %9lld\n",
                b.name.c_str(),
                human_bytes(b.rss_delta_bytes).c_str(),
                human_bytes(b.peak_rss_bytes).c_str(),
                b.bytes_per_event, b.symbols, b.cpu_util * 100.0, b.invol_switches);
    }

    //Per operation latency, from the instrumented pass. Kept apart from the table above
    //because it is a different run: the timestamp read costs a meaningful fraction of what
    //it measures, so folding it into the throughput figure would corrupt it.
    bool any_latency = false;

    for(size_t i = 0; i < r.books.size(); i++) {
        if(r.books[i].latency_present) {
            any_latency = true;
            break;
        }
    }

    if(!any_latency) {
        fprintf(out, "\n  per operation latency not measured on this run (pass "
                     "--latency)\n");

    } else {
        for(size_t i = 0; i < r.books.size(); i++) {
            const BookMetrics& b = r.books[i];

            if(!b.latency_present) {
                continue;
            }

            fprintf(out, "\n  %s - latency by message kind, ns per apply()\n",
                    b.name.c_str());

            //The floor under every number in the table below it. A p50 sitting at this
            //value means the instrument outran the book and the row is measuring itself.
            fprintf(out, "  instrument cost %.1f ns per sample%s\n",
                    b.instrument_ns,
                    (b.by_kind[OP_ADD].percentile(50) <= b.instrument_ns * 1.5)
                        ? "   <-- at or near this book's p50: not a measurement"
                        : "");

            fprintf(out, "  %-9s %12s %8s %8s %8s %8s %8s %10s\n",
                    "kind", "count", "p50", "p90", "p99", "p99.9", "max", "mean");

            for(int k = 0; k < OP_KIND_COUNT; k++) {
                format_latency_row(out, op_kind_name(static_cast<OpKind>(k)),
                                   b.by_kind[k]);
            }

            format_latency_row(out, "ALL", b.all_ops);
        }
    }

    //The exact half, same argument as the log database section
    bool any_ops = false;

    for(size_t i = 0; i < r.books.size(); i++) {
        if(r.books[i].ops_present) {
            any_ops = true;
            break;
        }
    }

    if(!any_ops) {
        fprintf(out,
                "\n  operation counters not exposed by any book on this run.\n"
                "\n"
                "  These are the figures that do not move between machines - level\n"
                "  probes, order hops, comparisons, id lookups, and for the heap the\n"
                "  tombstones a query had to step over. They are counted by the book\n"
                "  itself, so they arrive with the segment tree and heap backends.\n");
        return;
    }

    fprintf(out, "\n  operations performed (exact, machine independent)\n");
    fprintf(out, "  %-10s %12s %12s %12s %12s %10s\n",
            "book", "level prb", "order hops", "compares", "id probes", "tombstone");
    fprintf(out, "  %-10s %12s %12s %12s %12s %10s\n",
            "--------", "------------", "------------", "------------",
            "------------", "----------");

    for(size_t i = 0; i < r.books.size(); i++) {
        const BookMetrics& b = r.books[i];

        if(!b.ops_present) {
            continue;
        }

        fprintf(out, "  %-10s %12lld %12lld %12lld %12lld %10lld\n",
                b.name.c_str(), b.ops.level_probes, b.ops.order_hops,
                b.ops.comparisons, b.ops.id_probes, b.ops.tombstones_skipped);
    }

    fprintf(out, "\n  paging and cache\n");
    fprintf(out, "  %-10s %12s %12s %12s %12s %10s\n",
            "book", "page loads", "evictions", "hits", "misses", "hit rate");
    fprintf(out, "  %-10s %12s %12s %12s %12s %10s\n",
            "--------", "------------", "------------", "------------",
            "------------", "----------");

    for(size_t i = 0; i < r.books.size(); i++) {
        const BookMetrics& b = r.books[i];

        if(!b.ops_present) {
            continue;
        }

        fprintf(out, "  %-10s %12lld %12lld %12lld %12lld %9.1f%%\n",
                b.name.c_str(), b.ops.page_loads, b.ops.evictions,
                b.ops.cache_hits, b.ops.cache_misses,
                b.ops.cache_hit_rate() * 100.0);
    }
}


static void format_hardware(const RunReport& r, FILE* out) {
    section(out, "4. hardware");

    const HardwareMetrics& h = r.hardware;

    fprintf(out, "  machine state\n");
    fprintf(out, "    cpu                         %s\n", h.cpu_model.c_str());
    fprintf(out, "    ran on cpu                  %d (%s)\n",
            h.pinned_cpu, h.core_type.c_str());
    fprintf(out, "    governor                    %s\n", h.governor.c_str());
    fprintf(out, "    turbo                       %s\n",
            h.turbo_disabled ? "disabled" : "ENABLED (frequency will vary)");
    fprintf(out, "\n");

    if(!h.available) {
        fprintf(out, "  counters not collected on this run\n");
        fprintf(out, "    reason                    %s\n", h.unavailable_reason.c_str());
        fprintf(out, "\n");
        fprintf(out, "  Collect them with:\n\n    %s\n\n", h.command.c_str());
        fprintf(out,
                "  IPC is the headline: this core can retire ~6 instructions per cycle,\n"
                "  and a pointer chasing structure will land near 1. The top down\n"
                "  breakdown then says where the other five slots went - Retiring is the\n"
                "  only bucket that did useful work, and Memory Bound is the one this\n"
                "  study predicts will dominate.\n");
        return;
    }

    fprintf(out, "  instructions per cycle        %14.2f\n", h.ipc);
    fprintf(out, "\n  where the issue slots went\n");
    fprintf(out, "    retiring (useful work)      %13.1f%%\n", h.retiring_pct);
    fprintf(out, "    bad speculation             %13.1f%%\n", h.bad_speculation_pct);
    fprintf(out, "    frontend bound              %13.1f%%\n", h.frontend_bound_pct);
    fprintf(out, "    backend bound               %13.1f%%\n", h.backend_bound_pct);
    fprintf(out, "      memory bound              %13.1f%%\n", h.memory_bound_pct);
    fprintf(out, "      core bound                %13.1f%%\n", h.core_bound_pct);
    fprintf(out, "\n  counters\n");
    fprintf(out, "    cycles                      %14lld\n", h.cycles);
    fprintf(out, "    instructions                %14lld\n", h.instructions);
    fprintf(out, "    cache references            %14lld\n", h.cache_references);
    fprintf(out, "    cache misses                %14lld\n", h.cache_misses);
    fprintf(out, "    branch misses               %14lld\n", h.branch_misses);
    fprintf(out, "    dTLB load misses            %14lld\n", h.dtlb_load_misses);
}


void format_report(const RunReport& r, FILE* out) {
    rule(out, '=');
    fprintf(out, " order book engine - run report\n");
    rule(out, '=');

    fprintf(out, " started    %s\n", r.started_at.c_str());
    fprintf(out, " host       %s\n", r.host.c_str());
    fprintf(out, " command    %s\n", r.command_line.c_str());

    format_app(r, out);
    format_logdb(r, out);
    format_books(r, out);
    format_hardware(r, out);

    fprintf(out, "\n");
    rule(out, '=');
}


//Highest report_NNNN.txt already in the directory, or 0 if there are none. Scanned rather
//than tracked in a counter file: the directory IS the record, so a report copied in from
//another machine still takes part in the sequence.
static int highest_sequence(const string& dir) {
    DIR* d = opendir(dir.c_str());

    if(!d) {
        return 0;
    }

    int highest = 0;
    struct dirent* e;

    while((e = readdir(d)) != 0) {
        int n = 0;

        if(sscanf(e->d_name, "report_%d.txt", &n) == 1 && n > highest) {
            highest = n;
        }
    }

    closedir(d);

    return highest;
}


string write_report(const RunReport& r, const string& dir) {
    if(mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
        throw runtime_error("write_report: mkdir " + dir + ": " + strerror(errno));
    }

    char name[64];
    snprintf(name, sizeof(name), "/report_%04d.txt", highest_sequence(dir) + 1);

    const string path = dir + name;

    FILE* f = fopen(path.c_str(), "w");

    if(!f) {
        throw runtime_error("write_report: cannot open " + path);
    }

    format_report(r, f);

    if(fclose(f) != 0) {
        throw runtime_error("write_report: write failed for " + path);
    }

    return path;
}

} //namespace bench
