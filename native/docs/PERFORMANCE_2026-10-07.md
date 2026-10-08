# Performance notes, 2026-10-07

Machine: Intel i5-8265U (4 cores, 8 threads), Debian 13, GCC 14.2, X11. All timings are interleaved A/B runs, `perf stat -e task-clock` for CPU time.

## Gains so far this session

| Change | Result |
|---|---|
| Pointer motion repaints only when a hover target changes | 100 real moves over the terminal: 13 frames in total (was one per event) |
| Deferred compression of scrollback blocks (bounded scrollback) | 256 MB lxcat flood: 1.33 s -> 1.13 s wall, 4.9 -> 2.6 CPU-s (8 threads); 2 CPUs 1.7-1.9 s -> 1.48 s; 1 CPU 3.2 s -> 1.6 s |
| Same change, engine benchmark | 10,000-line scrollback about +10% |

| Compact scrollback lines (README row 30) | 256 MB ASCII log through lxcat: 0.50 s -> 0.31 s (0.83 GB/s), CPU 0.90 -> 0.75 s; mixed/non-ASCII text unchanged |
| Fast output for plain printers (README row 31) | `/bin/cat` of the 256 MB log through the pty: 3.05 s -> 0.95-1.04 s |

Earlier rounds (rows 1-27 of the README progression) are unchanged.

## Kernel findings (pty microbenchmark, `ptybench`: child writes 256 MB into a pty, parent reads)

| termios | line length | read size | throughput | avg bytes per read() |
|---|---|---|---|---|
| default (OPOST+ONLCR) | 20 | 64 KB | 0.04 GB/s | 82 |
| default | 100 | 64 KB | 0.09 GB/s | 356 |
| default | 100 | 1 MB | 0.09 GB/s | 350 |
| default | 5000 | 64 KB | 0.23 GB/s | 2110 |
| OPOST off | 20 / 100 / 5000 | 64 KB or 1 MB | 0.41-0.44 GB/s | about 9000 |

Reader buffer size and writer block size do not matter. The cost is the kernel's output post-processing (N_TTY handles a newline under ONLCR character by character with the output lock held, see https://docs.kernel.org/driver-api/tty/n_tty.html), which also breaks the stream into tiny reads. With OPOST off the ceiling is 4-10x higher. That is the basis of the fast-output switch. Switching it off for every program is not safe (programs that turn OPOST off themselves are raw-mode applications that expect a bare line feed), so it is done only while a known plain printer is in the foreground, and a watcher on a pidfd undoes it the moment the job exits, because readline in bash saves and restores the terminal settings around every prompt and would otherwise keep the switched-off state forever.

Windows: Lestrix is Linux-only; ConPTY does not apply. Bypassing the pty altogether is what `lxcat` already does for `cat FILE` (the data goes through a file, only a short escape sequence goes through the pty).

## Where the time goes now (perf record, lxcat flood)

lx-parse 46% (btask_build 33%, memmove 6%), lx-io 42% (btask_build 8%, memmove 7%, feed_serial 6.5%, hist_add 6%), compression is off the profile during the flood and runs when the terminal goes idle.

## Measured and rejected

| Idea | Result |
|---|---|
| Shorter or no `sched_yield` spin in `feed_sliced` | no change over 5 runs each |
| More compress or parse threads | same or slower; the IO thread is the serial limit |
| Scrollback copy on the parse workers (plan, parallel copy, link) | 1.40-1.52 s vs 1.33 s, more total CPU |
| LZ4-style skip acceleration in `sd_lz_compress` | no speed-up, CPU 5.5 vs 5.3 s |
| Row buffer written in place in `btask_build` (no row memmove/memset) | 1.08 s vs 1.07 s, within noise |
| BOLT 19 (`-Wl,-q` build, `perf record -j any,u`, `llvm-bolt -data=... -reorder-blocks=ext-tsp -reorder-functions=hfsort -split-functions -split-all-cold`) | ASCII log 0.30 s vs 0.30 s, random 1.13-1.17 s vs 1.06-1.20 s, 2 CPUs 1.67-1.71 s vs 1.62 s: no gain, binary 0.74 MB -> 6.4 MB |
| AutoFDO | not possible here: Debian autofdo 0.19 cannot read perf 6.12 data |
| `-flto`, `-O2`, `-mtune=native`, `-march=native`, `-funroll-loops`, alignment flags, `-fno-plt`, `-fno-pie`, `-fcf-protection=none`, PGO | no consistent win on the engine benchmark (best-of-12 identical); the engine is memory-bandwidth bound |

## Ideas not tried yet

- Plain `cat` through the pty is kernel-bound (52% of the time is inside `cat`); nothing to gain in userland.

## Hardware limits on this machine (bench/membench.c, bench/ptybench.c)

Intel i5-8265U, 4 cores / 8 threads, L1d 32 KB and L2 256 KB per core, shared L3, 40 GB RAM, governor `powersave` (about 3.4 GHz at the time). GB/s, buffers per thread, threads pinned to one hardware thread per core:

| Test (per-thread buffer) | 1 thread | 2 | 4 | 8 |
|---|---|---|---|---|
| memcpy in L1 (16 KB), read+write traffic | 192 | 384 | 193 | 178 |
| memcpy in L2 (128 KB) | 85 | 167 | 129 | 121 |
| memcpy in L3 (2 MB) | 53 | 36 | 22 | 22 |
| memcpy in RAM (64 MB) | 25.7 | 25.7 | 25.5 | 24.8 |
| read in RAM (AVX2 sum) | 14.2 | 19.9 | 21.5 | 19.4 |
| write in RAM (memset) | 29.3 | 26.6 | 29.2 | 27.4 |
| non-temporal write in RAM | 31.8 | 30.0 | 30.0 | 28.7 |

So RAM gives about 25 GB/s of copy traffic (12.5 GB/s of data copied), 14-21 GB/s of reads and 27-32 GB/s of writes, and one thread alone cannot read faster than about 14 GB/s. Shared L3 bandwidth collapses from 53 GB/s on one thread to 22 GB/s on four or more.

## What that says about Lestrix

- ASCII log flood, 256 MB in 0.30 s: about four passes over the data (file to buffer, parse to bytes, history copy, page cache) is roughly 1 GB of traffic, about 3.4 GB/s, which is 15-25% of what RAM can do. Reading the file once at 14 GB/s would take 18 ms; a floor with all passes is about 0.1 s. The remaining 3x is serial work on the IO thread (pread, newline scan, task split, history copy), not bandwidth.
- Random text with non-ASCII, 256 MB in 1.1 s: 2 GB of 8-byte cells written plus the copies is about 8 GB of traffic, 7 GB/s, about 30% of the limit. Earlier notes called this memory-bound; it is not at the RAM limit, it is limited by the single IO thread and by L3 contention between the parse threads.
- Plain pty: the kernel's newline processing, not memory (ptybench, 0.04-0.09 GB/s with it on, 0.41-0.44 with it off).

## CPU governor: powersave vs performance (same machine, same binaries)

`intel_pstate` is active, so only `powersave` and `performance` exist (no `schedutil`). Before: `powersave`, energy preference `balance_performance`, about 3.4 GHz under load (88% of the 3.9 GHz maximum). After: `performance`, energy preference `performance`, 3.70-3.78 GHz. Runs were taken in the same session with the same build; the powersave figures are the ranges measured earlier today.

| Test | powersave | performance | Change |
|---|---|---|---|
| ASCII log flood 256 MB, 8 threads | 0.29-0.33 s, CPU 0.69 s | 0.27-0.28 s, CPU 0.62 s | -8% |
| Random text flood 256 MB, 8 threads | 1.06-1.20 s (typically 1.13) | 1.02-1.12 s (typically 1.05) | -7% |
| Random text, pinned to 2 CPUs | 1.62-1.64 s | 1.51-1.53 s | -7% |
| ASCII log, 1 CPU, serial parser | 0.50 s | 0.44-0.46 s | -10% |
| `/bin/cat log \| cat` (pipe bypass) | 0.40 s (0.63 GB/s) | 0.36 s (0.70 GB/s) | -10% |
| `/bin/cat log` through the pty (fast output) | 0.95-1.04 s | 0.94 s (0.27 GB/s) | about -5% |
| pty benchmark, OPOST on / off, 100-char lines | 0.09 / 0.42 GB/s | 0.09 / 0.46 GB/s | kernel-bound |
| RAM memcpy traffic / write / NT write | 25.7 / 29.3 / 31.8 GB/s | 27.1 / 27.0 / 26.0 GB/s | unchanged (noise) |
| RAM read, 1 thread / 4 threads | 14.2 / 21.5 GB/s | 19.1 / 24.7 GB/s | one thread reads 35% faster |
| L1 memcpy, 1 thread | 192 GB/s | 203 GB/s | +6% |

So the governor is worth about 7-12% on everything the CPU does and nothing on the kernel pty path or RAM write bandwidth; the cause is clock speed, not a different bottleneck. A laptop on battery will not stay at `performance`, so the defaults and the measurements in the README stay on `powersave`; new `native/benchmark` runs record the governor on the first line of `summary.md`.

## Using the idle threads (governor `performance`)

| Step | ASCII log 256 MB | Random text 256 MB |
|---|---|---|
| Before (README row 32 build) | 0.27 s | 1.12 s |
| Stream files as bare line feeds, no conversion copy (row 33) | 0.22 s | 1.04 s |
| Pipelined parse/commit batches (row 34) | 0.21 s | 0.93 s |

| Escape pre-scan only after a stop, per-128-line limit checks, zero-copy all-compact blocks (row 35) | 0.18 s | 0.93 s |

Rejected: 1, 2 and 4 MB feed slices instead of 512 KB (ASCII -5% at best, random text +15%).

## Serial parser and speed gate (2026-10-08, README row 36)

Method: callgrind (perf is blocked here, `perf_event_paranoid=3`), A/B against `git show HEAD:native/src/vt.c` built with the same flags, best of 5-6 runs of `tests/perf_gate.c` and a scratch driver (8 MB of CRLF lines of 20/85/170/1000 characters, 176x49 grid).

| Case | Before | After |
|---|---|---|
| Gate, no scrollback | 1429 MB/s | about 1580 MB/s |
| Gate, 10,000-line scrollback | 690 MB/s | about 790 MB/s |
| 85-char lines, scrollback | 605 MB/s | about 877 MB/s |
| 170-char lines, scrollback | 673 MB/s | about 1130 MB/s |
| 20-char lines, scrollback | 390 MB/s | 385-430 MB/s (kept on the old path below 32 cells) |
| Text with no line breaks (autowrap), no scrollback | about 2.1 GB/s | unchanged, the floor for this path |

Kept: (1) `hist_push` packs all-ASCII single-style rows of 32-512 cells into `hist_add_compact` (1 byte per character; the 8-byte copy and the later 8-byte block packing both disappear); (2) `\r` and `\n` after an ASCII run handled inline in `feed_serial`.
Rejected on the gate: `-march=native` 1369, LTO 1399, PGO 1405, `-freorder-blocks-algorithm=simple -fvect-cost-model=unlimited` 1386 (baseline 1426); 16-bytes-per-step `fill_ascii` (no gain, short runs -10%); compacting rows under 32 cells (-20%).
Remaining gap: the scrollback path is still about 2.6x below the 2.1 GB/s floor (history compaction pass over each row, block packing). Not measured: GUI benchmark, input latency, 1024x768 (parse engine only).
Tests after the change: test-vt 120/0, test-lz 1005/0, test_fast_paths (ASan/UBSan differential) 34163/0, TSan 120/0.
