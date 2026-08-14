#include "bench/resources.h"

#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#endif

using namespace std;


#if defined(__linux__)

//Pull one "Key:  N kB" line out of /proc/self/status. Returns -1 if the key is absent,
//which happens on kernels that do not expose it rather than being an error.
static long long status_kb(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");

    if(!f) {
        return -1;
    }

    size_t keylen = strlen(key);
    char line[256];
    long long kb = -1;

    while(fgets(line, sizeof(line), f)) {
        if(strncmp(line, key, keylen) == 0) {
            //Skip the key and its colon, then let scanf find the number
            if(sscanf(line + keylen, ": %lld", &kb) != 1) {
                kb = -1;
            }

            break;
        }
    }

    fclose(f);
    return kb;
}


//Pull one "key: N" line out of /proc/self/io. Unlike status this is in bytes already.
//
//The file is unreadable when the kernel is built without CONFIG_TASK_IO_ACCOUNTING, and on
//some hardened setups it needs privileges the process does not have. Both come back -1.
static long long io_bytes(const char* key) {
    FILE* f = fopen("/proc/self/io", "r");

    if(!f) {
        return -1;
    }

    size_t keylen = strlen(key);
    char line[256];
    long long n = -1;

    while(fgets(line, sizeof(line), f)) {
        if(strncmp(line, key, keylen) == 0) {
            if(sscanf(line + keylen, ": %lld", &n) != 1) {
                n = -1;
            }

            break;
        }
    }

    fclose(f);
    return n;
}

#endif


CpuSnapshot snapshot_cpu() {
    CpuSnapshot c;

#if defined(__linux__)
    //Process wide, so this counts every thread. Single threaded here by construction, but
    //if the universe is ever sharded across threads this stays the right figure.
    struct timespec ts;

    if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
        c.cpu_ns = static_cast<long long>(ts.tv_sec) * 1000000000LL
                 + static_cast<long long>(ts.tv_nsec);
    }

    struct rusage ru;

    if(getrusage(RUSAGE_SELF, &ru) == 0) {
        c.minor_faults = ru.ru_minflt;
        c.major_faults = ru.ru_majflt;
        c.vol_switches = ru.ru_nvcsw;
        c.invol_switches = ru.ru_nivcsw;
    }
#endif

    return c;
}


ResourceSnapshot snapshot_resources() {
    ResourceSnapshot s;

#if defined(__linux__)
    long long rss_kb = status_kb("VmRSS");
    long long hwm_kb = status_kb("VmHWM");

    s.rss_bytes = (rss_kb >= 0) ? rss_kb * 1024 : -1;
    s.peak_rss_bytes = (hwm_kb >= 0) ? hwm_kb * 1024 : -1;

    CpuSnapshot c = snapshot_cpu();
    s.cpu_ns = c.cpu_ns;
    s.minor_faults = c.minor_faults;
    s.major_faults = c.major_faults;
    s.vol_switches = c.vol_switches;
    s.invol_switches = c.invol_switches;

    s.read_chars = io_bytes("rchar");
    s.write_chars = io_bytes("wchar");
    s.read_bytes = io_bytes("read_bytes");
    s.write_bytes = io_bytes("write_bytes");
#endif

    return s;
}


DiskSpace snapshot_disk(const string& path) {
    DiskSpace d;

#if defined(__linux__)
    struct statvfs vfs;

    if(statvfs(path.c_str(), &vfs) == 0) {
        //f_frsize is the fragment size, which is what the block counts are expressed in.
        //f_bsize is the preferred I/O size and is NOT interchangeable with it.
        long long unit = static_cast<long long>(vfs.f_frsize);

        d.total_bytes = static_cast<long long>(vfs.f_blocks) * unit;
        d.free_bytes = static_cast<long long>(vfs.f_bfree) * unit;
        d.avail_bytes = static_cast<long long>(vfs.f_bavail) * unit;
    }
#else
    (void)path;
#endif

    return d;
}


long long file_size(const string& path) {
#if defined(__linux__)
    struct stat st;

    if(stat(path.c_str(), &st) == 0) {
        return static_cast<long long>(st.st_size);
    }
#else
    (void)path;
#endif

    return -1;
}


//Binary units, because this reports memory and block counts rather than marketing figures.
//One decimal place keeps a column narrow while still separating 512.3 M from 512.9 M.
string format_bytes(long long bytes) {
    if(bytes < 0) {
        return "n/a";
    }

    const char* unit[] = { "B", "K", "M", "G", "T" };

    double v = static_cast<double>(bytes);
    int u = 0;

    while(v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }

    char buf[64];

    //Whole bytes have no meaningful fraction, so print them as an integer
    if(u == 0) {
        snprintf(buf, sizeof(buf), "%lld B", bytes);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", v, unit[u]);
    }

    return string(buf);
}
