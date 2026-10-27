/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef LX_TRACE_H
#define LX_TRACE_H

/* LX_TRACE=1 records timestamped events from every thread into a fixed array and prints them, sorted, to stderr at exit
 * (lx_trace_dump). Off: one relaxed load and a branch per call site. Event names must be string literals. */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <sys/syscall.h>
#include <unistd.h>

namespace lx {

struct TraceEv { uint64_t ns; int tid; const char *what; long arg; };
enum { TRACE_MAX = 1 << 18 };

inline std::atomic<int> &trace_on() { static std::atomic<int> on{-1}; return on; }
inline std::atomic<unsigned> &trace_n() { static std::atomic<unsigned> n{0}; return n; }
inline TraceEv *trace_buf() { static TraceEv *b = static_cast<TraceEv*>(calloc(TRACE_MAX, sizeof(TraceEv))); return b; }

inline void trace(const char *what, long arg = 0) {
    int on = trace_on().load(std::memory_order_relaxed);
    if (on == 0) return;
    if (on < 0) { on = getenv("LX_TRACE") != nullptr && trace_buf() != nullptr; trace_on().store(on); if (!on) return; }
    unsigned i = trace_n().fetch_add(1, std::memory_order_relaxed);
    if (i >= TRACE_MAX) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    trace_buf()[i] = {(uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, (int)syscall(SYS_gettid), what, arg};
}

inline void trace_dump() {
    if (trace_on().load() <= 0) return;
    unsigned n = std::min<unsigned>(trace_n().load(), TRACE_MAX);
    TraceEv *b = trace_buf();
    std::sort(b, b + n, [](const TraceEv &x, const TraceEv &y) { return x.ns < y.ns; });
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "TRACE %.3f ms tid %d %s %ld\n", (double)(b[i].ns - b[0].ns) / 1e6, b[i].tid, b[i].what, b[i].arg);
}

}

#endif
