//Turning a RunReport into something a person reads, and filing it.
//
//The report is written to a directory rather than only to the terminal because the
//figures it carries are the deliverable, and a figure that scrolled past in a terminal
//cannot be diffed against the same figure from last week. MR-3 wants every number
//reproducible from a documented command on a documented input; a filed report carries
//that command, the capture, and the machine state next to the numbers it produced.
//
//Reports are never overwritten. The name carries a sequence and the next one takes the
//next number, so a run cannot quietly destroy the evidence from the run before it - which
//matters most in exactly the situation where it would hurt, re-running to chase a
//regression.

#ifndef BENCH_REPORT_H
#define BENCH_REPORT_H

#include "bench/metrics.h"

#include <cstdio>
#include <string>

namespace bench {

//Fill in what can be known about the machine without perf: model, the CPU this run is
//pinned to and whether it is a performance or an efficiency core, the governor, and
//whether the counters are readable at all. On a hybrid part the core type alone moves IPC
//enough to invalidate a comparison, so it is recorded whether or not perf ran.
void probe_hardware(HardwareMetrics& hw);

//The perf invocation that would fill the rest of the hardware section, built to match
//this run so the report is reproducible from itself.
std::string perf_command(const std::string& argv0, const std::string& args, int cpu);

//Format the whole report.
void format_report(const RunReport& r, FILE* out);

//Write it under `dir`, creating the directory if it is not there, as report_NNNN.txt with
//NNNN one past the highest already present. Returns the path written.
std::string write_report(const RunReport& r, const std::string& dir);

} //namespace bench

#endif
