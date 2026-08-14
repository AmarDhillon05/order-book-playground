//Timing harness. One replay of a preloaded event stream through one book iteration.
//
//The stream is decoded off the pcap once, up front, and held in memory. Nothing in the
//timed region touches libpcap, allocates the input, or formats output, so what is being
//measured is the book and only the book.

#ifndef BENCH_H
#define BENCH_H

#include "bench/metrics.h"
#include "bench/resources.h"
#include "books/book.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct BenchResult {
    std::string name;
    long long events;
    double best_ns;                 //Fastest repetition, total nanoseconds
    double worst_ns;
    double mean_ns;
    int64_t checksum;

    //--- resource use ---------------------------------------------------------------------
    //
    //All of it read outside the clock, per MR-2: a snapshot before the region and one after,
    //never within. The /proc reads sit outermost where they cost the least.

    //Resident set the book added, measured on the FIRST repetition only.
    //
    //Later repetitions have no clean baseline: glibc does not reliably return freed pages to
    //the kernel, so a book destroyed at the end of rep 0 leaves RSS elevated and rep 1's
    //delta reads far too low. Rep 0 is the only honest measurement of what a book costs.
    long long rss_delta_bytes;

    long long peak_rss_bytes;       //Process high water mark, after every repetition
    double bytes_per_event;
    long long bytes_per_symbol;     //The multi-book metric: cost of holding one instrument
    size_t symbols;                 //Instruments held at the end of rep 0

    //From the repetition that produced best_ns, so they explain that run and not an average
    //of runs with different behaviour.
    long long cpu_ns;
    double cpu_util;                //cpu_ns / best_ns. ~1.0 expected; a DIP is the signal
    long long minor_faults;
    long long major_faults;
    long long invol_switches;

    //Disk. Nothing writes yet, so the write figures are a baseline for a future store
    //rather than a present result.
    long long read_bytes;           //Actually fetched from the block device
    long long write_bytes;
    long long read_chars;           //Requested through syscalls; compare to read_bytes
    long long write_chars;
};

//Load the capture into memory, ready to replay. Stops after `limit` events, or reads the
//whole file when limit <= 0.
std::vector<Event> load_events(const std::string& path, long long limit);


//Replay `events` through a fresh Book, `reps` times, keeping the best run.
//
//A new Book is constructed per repetition so that every run starts from an empty book
//and sees the same work. Construction and destruction sit outside the clock: teardown of
//a large book is real cost, but it is not ingestion cost, and charging it here would
//punish whichever backend happens to hold the most nodes at the end.
template <class Book>
BenchResult run_book(const std::string& name, const std::vector<Event>& events, int reps) {
    using clock = std::chrono::steady_clock;

    //Check the book against books/book.h before anything else. Instantiating the
    //template is what fires its static_asserts, so a book missing part of the contract
    //names the missing method here rather than failing somewhere deeper.
    (void)sizeof(BookContract<Book>);

    BenchResult r;
    r.name = name;
    r.events = static_cast<long long>(events.size());
    r.best_ns = 0;
    r.worst_ns = 0;
    r.mean_ns = 0;
    r.checksum = 0;
    r.rss_delta_bytes = -1;
    r.peak_rss_bytes = -1;
    r.bytes_per_event = 0;
    r.bytes_per_symbol = -1;
    r.symbols = 0;
    r.cpu_ns = -1;
    r.cpu_util = 0;
    r.minor_faults = -1;
    r.major_faults = -1;
    r.invol_switches = -1;
    r.read_bytes = -1;
    r.write_bytes = -1;
    r.read_chars = -1;
    r.write_chars = -1;

    double total = 0;

    for(int rep = 0; rep < reps; rep++) {
        //Before the book exists, so its construction counts toward the memory delta
        ResourceSnapshot before = snapshot_resources();

        Book book;

        //Bracketed as tightly as the wall clock allows, and OUTSIDE it: two syscalls that
        //neither allocate nor do I/O, but still instrumentation, so they stay off the
        //measured path per MR-2.
        //
        //The consequence is that the CPU window is a hair wider than the wall window, so
        //utilisation can read a few hundredths over 100%. That is instrument skew, not a
        //reading. It was 0.8% before this was tightened, because book construction and
        //get_checksum() were both inside the CPU window and outside the wall one.
        CpuSnapshot c0 = snapshot_cpu();
        clock::time_point t0 = clock::now();

        for(size_t i = 0; i < events.size(); i++) {
            book.apply(events[i]);
        }

        clock::time_point t1 = clock::now();
        CpuSnapshot c1 = snapshot_cpu();

        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

        //Read the book after the clock stops. Without this the compiler is free to
        //notice nothing observes the state and delete the loop that built it.
        //
        //Before the snapshot, deliberately: get_checksum() is O(symbols) and allocates
        //nothing, so it cannot move RSS, whereas get_all_tops() below builds a vector per
        //instrument and would be charged to the book if it ran first.
        r.checksum = book.get_checksum();

        ResourceSnapshot after = snapshot_resources();

        //Instrument count through the contract rather than through a book specific
        //accessor, so this works for every backend including the control
        std::vector<NamedTop> tops;
        book.get_all_tops(tops);

        total += ns;

        if(rep == 0) {
            //Rep 0 only - see the note on rss_delta_bytes. Nothing after this repetition
            //has a clean baseline to subtract from.
            if(before.rss_bytes >= 0 && after.rss_bytes >= 0) {
                r.rss_delta_bytes = after.rss_bytes - before.rss_bytes;

                if(r.events > 0) {
                    r.bytes_per_event =
                        static_cast<double>(r.rss_delta_bytes) / r.events;
                }

                if(!tops.empty()) {
                    r.bytes_per_symbol =
                        r.rss_delta_bytes / static_cast<long long>(tops.size());
                }
            }

            r.symbols = tops.size();
        }

        if(rep == 0 || ns < r.best_ns) {
            r.best_ns = ns;

            //From the tight CPU bracket, not from the /proc snapshots, so these describe
            //the replay loop rather than the loop plus construction plus teardown
            if(c0.cpu_ns >= 0 && c1.cpu_ns >= 0) {
                r.cpu_ns = c1.cpu_ns - c0.cpu_ns;
                r.cpu_util = (ns > 0) ? static_cast<double>(r.cpu_ns) / ns : 0.0;
            }

            if(c0.minor_faults >= 0) {
                r.minor_faults = c1.minor_faults - c0.minor_faults;
                r.major_faults = c1.major_faults - c0.major_faults;
                r.invol_switches = c1.invol_switches - c0.invol_switches;
            }

            if(before.read_bytes >= 0) {
                r.read_bytes = after.read_bytes - before.read_bytes;
                r.write_bytes = after.write_bytes - before.write_bytes;
                r.read_chars = after.read_chars - before.read_chars;
                r.write_chars = after.write_chars - before.write_chars;
            }
        }

        if(ns > r.worst_ns) {
            r.worst_ns = ns;
        }
    }

    r.mean_ns = total / reps;

    //After every repetition, so this is the whole run's high water mark
    r.peak_rss_bytes = snapshot_resources().peak_rss_bytes;

    return r;
}


//One book's per-operation latency distribution, split by message kind.
struct LatencyResult {
    bench::LatencyHist by_kind[bench::OP_KIND_COUNT];
    bench::LatencyHist all;

    //What one timestamp pair cost, measured on this machine on this run. Reported, never
    //subtracted - see the note in metrics.h.
    double instrument_ns;

    LatencyResult() : instrument_ns(0) {}
};


//Replay once, timing every apply() individually.
//
//This is DELIBERATELY a different run from run_book(), and the two figures must not be
//compared. Bracketing every call costs a timestamp pair per event - around 7 ns here,
//against a control book that applies an event in 6 - so this run is slower than the
//throughput run by a margin that is large for a cheap book and small for an expensive
//one. That is not a defect to correct for; it is why the two numbers are collected apart.
//
//What survives the overhead is the SHAPE: the split between message kinds, and the tail.
//A p99.9 that sits ten times the p50 is a real finding whether or not every sample
//carries a constant 7 ns.
//
//Nothing here allocates: the histograms are sized at construction and a sample is an
//increment, so MR-2 holds on the measured path.
template <class Book>
LatencyResult run_book_latency(const std::vector<Event>& events) {
    (void)sizeof(BookContract<Book>);

    LatencyResult r;

    //Calibrated before the loop, never inside it
    const double per_ns = bench::tsc_ticks_per_ns();
    r.instrument_ns = bench::calibrate_instrument_ns();

    Book book;

    for(size_t i = 0; i < events.size(); i++) {
        const bench::OpKind k = bench::op_kind_of(events[i].kind);

        const uint64_t t0 = bench::read_tsc();
        book.apply(events[i]);
        const uint64_t t1 = bench::read_tsc();

        const uint64_t ns = static_cast<uint64_t>(
            static_cast<double>(t1 - t0) / per_ns);

        r.by_kind[k].add(ns);
        r.all.add(ns);
    }

    //Read the book so the optimizer cannot delete the loop that built it, same reason as
    //in run_book()
    if(book.get_checksum() == 0x7FFFFFFFFFFFFFFFLL) {
        fprintf(stderr, " ");
    }

    return r;
}

#endif
