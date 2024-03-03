#pragma once

// Michal Vlasák, FIT CTU, 2024

// This file hosts some utilities which are general enough to warrant a separate
// file - since they are either applicable to multiple parts of the template, or
// could be useful even if the rest of the template is not used.
//
// Currently there are:
//
// 1) Basic type aliases for fixed size integers (e.g. `u32` is `uint32_t`).
//
// 2) `unreachable` function to assert that a place in code is not reached
//    (without invoking undefined behavior).
//
// 3) Macros for marking allocated memory as "poisoned" for the Address
//    Sanitizer. These are very useful for custom memory allocation, since if we
//    allocated a big chunk of memory with malloc, Address Sanitizer with regard
//    it as fully available to the program, whereas we might be giving out the
//    memory from this chunk by smaller pieces. With the concept of "poison", we
//    can first mark the malloc'ed space as poisoned, and unlock unpoison the
//    parts of the chunk that we give away, and potentially on later free poison
//    them again.
//
//    There still may be hidden issues where there is a write out of bounds in
//    one small allocation, which is actually in bounds of the following
//    allocation. But poisoning still provides enough benefits to be
//    worthwhile, espicially if we have wrapping macros, which are no-ops if we
//    are not using the Address Sanitzer.

#include <cinttypes>
#include <cstdlib>
#include <source_location>
#include <iostream>
#include <chrono>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

[[noreturn]] inline void unreachable(std::source_location location = std::source_location::current()) {
	std::clog << "ERROR: unreachable code reached at "
		<< location.file_name() << ':' << location.line() << std::endl;
	exit(1);
}


inline long long nanos_since_epoch() {
	auto now = std::chrono::system_clock::now();
	auto now_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(now);
	return now_ns.time_since_epoch().count();
}

// ASAN integeration

#ifndef __has_feature
#define __has_feature(x) 0
#endif

extern "C" {

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
// https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define ASAN_POISON_MEMORY_REGION(addr, size) \
        __asan_poison_memory_region((addr), (size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) \
        __asan_unpoison_memory_region((addr), (size))
#else
#define ASAN_POISON_MEMORY_REGION(addr, size) \
        ((void) (addr), (void) (size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) \
        ((void) (addr), (void) (size))
#endif

}
