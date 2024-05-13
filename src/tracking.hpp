#pragma once

// Michal Vlasák, FIT CTU, 2024

// In this file is an implementation of memory usage tracking, and its logging
// into a so called "VM log" file. The VM log file may serve us later for
// analysis and comparison.
//
// There were a few design goals for the implementation: precision, low
// overhead, flexibility, unintrusiveness.
//
// The VM log is a simple CSV file that contains the header line, followed by a
// line per "logged event". Example:
//
//     # example.lox
//     class A {}
//     print A();
//
//     $ LOX_MEM_SAMPLE_THRESHOLD=200 LOX_VMLOG=vmlog lox example.lox
//
//     # vmlog
//     timestamp,event,static_mem,heap_mem
//     1709417723930733472,VM START,0,0
//     1709417723930756747,Parser START,0,0
//     1709417723930846089,Threshold,224,0
//     1709417723930856320,Parser END,240,0
//     1709417723931314352,Threshold,240,272
//     1709417723931903268,Threshold,240,488
//     1709417723932505183,Threshold,240,0
//     1709417723932684651,Threshold,0,0
//     1709417723932706180,VM END,0,0
//
// Each logged event is timestamped with Unix time in nanoseconds, has an event
// name and lists the memory usage (in bytes) for the two memory categories we
// care about.
//
// Log events can be either explicit, to for example mark interesting points in
// the life of VM (like start/end of the VM, stages of parsing, compilation
// and execution, garbage collection, etc.). Currently stages use the convention
// of the events being named "stage START" and "stage END" for the start and end
// events respectively.
//
// To provide more exact insight into memory usage, without logging each
// allocation and deallocation, we employ Threshold-based memory sampling
// inspired by Scalene. We keep track of memory allocated and deallocated since
// the last log, and if their sum (i.e. the change in memory usage) surpasses a
// threshold, then a "Threshold" log event is written to the log file. Since
// this threshold is configurable by an environment variable
// `LOX_MEM_SAMPLE_THRESHOLD`, the tracking can do anything ranging from loggin
// each allocation and deallocation (threshold=0), to not running at all. The
// default threshold is 10 KiB.
//
// The log file is only created if the environment variable `LOX_VMLOG`
// specifies the destination file for the log output. This file is first
// truncated, and then the log entries are written until the end of execution.
//
// There is only one "singleton" `VMLogger` object, which can be accessed
// through the `VMLogger::get` method. Through this object you can update the
// memory usage for the tracked memory kinds:
//
//     VMLogger::increase_memory_usage
//     VMLogger::decrease_memory_usage
//
// or log your own events with `VMLogger::log`.

#include <cstring>
#include <cstddef>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <charconv>

#include "utils.hpp"

enum MemoryKind {
	MemoryKind_Static, // AST, code, constants, string pool, stacks, etc.
	MemoryKind_Heap, // Heap memory
	MemoryKind_COUNT,
};

#ifdef __GNUC__
#define WEAK __attribute__ ((__weak__))
#else
#define WEAK
#endif

// man mallinfo2
struct mallinfo2
{
  size_t arena;    /* non-mmapped space allocated from system */
  size_t ordblks;  /* number of free chunks */
  size_t smblks;   /* number of fastbin blocks */
  size_t hblks;    /* number of mmapped regions */
  size_t hblkhd;   /* space in mmapped regions */
  size_t usmblks;  /* always 0, preserved for backwards compatibility */
  size_t fsmblks;  /* space available in freed fastbin blocks */
  size_t uordblks; /* total allocated space */
  size_t fordblks; /* total free space */
  size_t keepcost; /* top-most, releasable (via malloc_trim) space */
};

extern "C" struct mallinfo2 WEAK mallinfo2(void);

class MemoryTracker {
public:
	bool increase(u64 amount, u64 threshold) {
		increment += amount;
		if (increment >= threshold - decrement) {
			update();
			return true;
		}
		return false;
	}

	bool decrease(u64 amount, u64 threshold) {
		decrement += amount;
		if (decrement >= threshold - increment) {
			update();
			return true;
		}
		return false;
	}

	u64 usage() {
		update();
		return last_reported;
	}

private:
	void update() {
		last_reported += increment - decrement;
		increment = decrement = 0;
	}

	u64 last_reported {};
	u64 increment {};
	u64 decrement {};
};

class VMLogger {
public:
	VMLogger() : start_nanos_since_epoch(nanos_since_epoch())
	{
		const char *threshold = std::getenv("LOX_MEM_SAMPLE_THRESHOLD");
		if (threshold && threshold[0]) {
			size_t len = strlen(threshold);
			long long value = mem_increase_threshold;
			std::from_chars_result res = std::from_chars(threshold, threshold + len, value);
			if (res.ptr != threshold) {
				mem_increase_threshold = value;
			}
		}
		const char *log_path = std::getenv("LOX_VMLOG");
		if (log_path && log_path[0]) {
			log_file = std::ofstream(log_path);
		}
		log_file << "timestamp,event,static_mem,heap_mem,malloc_mem,rss_anon_mem\n";
		log("VM START");
	}

	~VMLogger() {
		log("VM END");
	};

	static VMLogger &get() {
		static VMLogger logger{};
		return logger;
	}

	void log(const char *event_name) {
		if (!log_file) {
			return;
		}

		struct mallinfo2 mi = {};
		if (mallinfo2) {
			mi = mallinfo2();
		}

		size_t rssanon = 0;
		FILE* f = fopen("/proc/self/status", "r");
		if (f != nullptr) {
			char buf[256];
			while (fgets(buf, sizeof(buf), f)) {
				if (sscanf(buf, "RssAnon: %zu kB", &rssanon) == 1) {
					break;
				}
			}
			fclose(f);
		}

		log_file << nanos_since_epoch()
			<< ',' << event_name
			<< ',' << trackers[MemoryKind_Static].usage()
			<< ',' << trackers[MemoryKind_Heap].usage()
			<< ',' << mi.uordblks + mi.hblkhd
			<< ',' << rssanon * 1024
			<< '\n';
	}

	void increase_memory_usage(MemoryKind kind, size_t amount) {
		if (trackers[kind].increase(amount, mem_increase_threshold)) {
			log("Threshold");
		}
	}

	void decrease_memory_usage(MemoryKind kind, size_t amount) {
		if (trackers[kind].decrease(amount, mem_increase_threshold)) {
			log("Threshold");
		}
	}

	long long nanos_since_start() const {
		return nanos_since_epoch() - start_nanos_since_epoch;
	}

private:
	std::ofstream log_file;
	u64 mem_increase_threshold { 1048576 };
	long long start_nanos_since_epoch;
	MemoryTracker trackers[MemoryKind::MemoryKind_COUNT];
};
