//Process resource readings, taken around a measured region rather than inside it.
//
//Everything here is a syscall or a /proc read, so none of it may happen on the measured
//path - MR-2. The discipline is that a snapshot is taken before the clock starts and after
//it stops, never between, and the /proc read sits outermost where it costs the least.
//
//WHAT CPU UTILISATION DOES AND DOES NOT TELL YOU
//
//The replay is single threaded, so utilisation is ~100% whenever the process is actually
//running and the number is uninteresting on its own. What it is good for is the DIP: a
//figure below 100% means wall time passed that the process did not spend on a CPU, which is
//page faulting, I/O, or being descheduled. Read alongside the fault and context switch
//counts it explains a slow repetition; read alone it says nothing about how well the code
//uses the machine.
//
//Real efficiency - instructions per cycle, cache miss rates, branch misses - is NOT here and
//deliberately so. Those need hardware counters, and spec.md asks for them via `perf stat`
//around the binary, which is more accurate than anything self-instrumentation would produce
//and does not put perf_event_open on the measured path. Use:
//
//  perf stat -e cycles,instructions,cache-misses,branch-misses ./bookbench <capture> ...

#ifndef BENCH_RESOURCES_H
#define BENCH_RESOURCES_H

#include <string>

//One reading. Fields that the platform cannot supply come back as -1 so that a missing
//figure prints as "n/a" rather than as a convincing zero.
struct ResourceSnapshot {
    long long rss_bytes;            //Resident set right now
    long long peak_rss_bytes;       //High water mark for the process lifetime
    long long cpu_ns;               //Process CPU time, user + system

    long long minor_faults;         //Served without I/O: usually first touch of new pages
    long long major_faults;         //Required I/O. On a replay these mean the file or swap
    long long vol_switches;         //Gave up the CPU, typically waiting on something
    long long invol_switches;       //Was preempted. The usual cause of a slow repetition

    //--- disk ---
    //
    //Two levels, and the difference between them is the point. read_chars is what the code
    //ASKED for through read()/write(); read_bytes is what actually went to the block device.
    //
    //  read_bytes  <<  read_chars      the page cache served it. Cheap.
    //  read_bytes  ~=  read_chars      every request hit the disk.
    //  write_bytes  >  write_chars     write amplification - the store is rewriting more
    //                                  than it was handed, which is what a B-tree or an
    //                                  LSM compaction does and is the number a persistence
    //                                  layer has to be judged on.
    //
    //Nothing here writes yet, so the write figures are a baseline to measure a future store
    //against rather than a current result.
    long long read_chars;           //Bytes requested via syscalls
    long long write_chars;
    long long read_bytes;           //Bytes actually fetched from the block device
    long long write_bytes;

    ResourceSnapshot()
        : rss_bytes(-1), peak_rss_bytes(-1), cpu_ns(-1),
          minor_faults(-1), major_faults(-1),
          vol_switches(-1), invol_switches(-1),
          read_chars(-1), write_chars(-1), read_bytes(-1), write_bytes(-1) {}
};


//Space on the filesystem holding a given path. Reported so a run that is about to write a
//database says up front whether there is room, rather than failing partway through.
struct DiskSpace {
    long long total_bytes;
    long long free_bytes;           //Free overall
    long long avail_bytes;          //Free to an unprivileged process, which is the real limit

    DiskSpace() : total_bytes(-1), free_bytes(-1), avail_bytes(-1) {}
};


//The subset that costs only syscalls, with no /proc read behind it.
//
//Kept separate because the full snapshot opens and parses two files, which is far too
//expensive to sit tight against a timed region - and if it is not tight, the CPU window ends
//up wider than the wall window and utilisation reads over 100%.
struct CpuSnapshot {
    long long cpu_ns;
    long long minor_faults;
    long long major_faults;
    long long vol_switches;
    long long invol_switches;

    CpuSnapshot()
        : cpu_ns(-1), minor_faults(-1), major_faults(-1),
          vol_switches(-1), invol_switches(-1) {}
};


//Two syscalls, no allocation, no I/O. Cheap enough to bracket a measured region closely.
CpuSnapshot snapshot_cpu();


//Read the current resource state. Cheap, but not free: two syscalls and two small /proc
//reads, so call it around a timed region and never within one.
ResourceSnapshot snapshot_resources();


//Filesystem space where `path` lives. Path need not be writable, only resolvable.
DiskSpace snapshot_disk(const std::string& path);


//Size of one file in bytes, or -1 if it cannot be stat'd. For reporting how large an input
//capture, or a future on disk store, actually is.
long long file_size(const std::string& path);


//Bytes as a short human readable string, e.g. "512.3 M". Fixed width friendly.
//A negative input, meaning the platform did not supply the figure, formats as "n/a".
std::string format_bytes(long long bytes);

#endif
