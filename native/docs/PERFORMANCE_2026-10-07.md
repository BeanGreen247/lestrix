# Performance notes, 2026-10-07

Machine: Intel i5-8265U (4 cores, 8 threads), Debian 13, GCC 14.2, X11. All timings are interleaved A/B runs, `perf stat -e task-clock` for CPU time.

## Index of every optimization (applied and dropped), newest last

Rule: every optimization, applied or dropped, gets an entry here with before/after numbers. Details are in the sections named in the last column.

| Date | Commit | Optimization | Status | Detail |
|---|---|---|---|---|
| 2026-10 | 055ee9f | Compact scrollback, deferred compression, fast output, pipelined parser, lite mode | applied | Gains so far |
| 2026-10 | bb77cac | Compact scrolled rows on the serial path, inline CR/LF after ASCII runs | applied | Serial parser and speed gate |
| 2026-10-08 | 51f9cd7 | Shadow ASCII rows for scrollback, splice in lxcat, input queue head offset | applied | Gaps 1, 3, 6 |
| 2026-10-08 | 9af718a | Fast output for 43 more plain printers | applied | Remaining gaps |
| 2026-10-08 | 7cca07a | inotify for the theme, idle sleep to the next timer | applied | Idle wake-ups |
| 2026-10-08 | - | lxcat hand-off polling via inotify; `sched_yield` spin in `feed_sliced` | dropped | Gaps 4, 5 |
| 2026-10 | 0b24169, bd475d4 | SGR straight to the pen, pack scan stops at the first second style, style cache skipped for the default pen, quick CUP, memmove for the scroll order | applied | Serial parser and speed gate |
| 2026-10 | e2c8ebd, 0289c8e, 9c735b9 | UI: clear only used rows, cursor stops blinking when idle, on-demand job threads, adaptive frame cap, echo without frame gap (16.9 to 2.9 ms) | applied | Idle wake-ups, README |
| 2026-10-27 | 2c94b47 | Full-screen scroll slides a window over the row order | applied | region_up row-order window |
| 2026-11-03 | 5c6a55c..81b2f71 | Colour emoji atlas, CPU renderer, changed-tiles repaint, UI wake-up storm fix (UI thread CPU 4x lower) | applied | Round 2026-11-03 |
| 2026-11-05 | 944dee3 | Read delay 200 to 400 us | applied | Read delay |
| 2026-11-09 | 0.4.2 | `sgr_quick` pairs, 256-colour and truecolor fast paths (colour 418 to 450 MB/s, 256/truecolor 257 to 307) | applied | Colour parsing round |
| 2026-11-09 | 0.4.2 | AVX2 packing loop in `hist_push` (runtime dispatch), colour driver about 281-290 to 296-301 MB/s | applied | Gap hunt round 2 |
| 2026-11-09 | - | Pack scrollback bytes straight into the history segment (no second copy) | dropped | Gap hunt round 2 |
| 2026-11-09 | - | Cheaper `hist_add` (cell copy into history) | dropped | Gap hunt round 2 |
| 2026-11-09 | - | One-entry style cache in front of `intern_style` | dropped | Gap hunt round 2 |

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

## Gap 1: scrollback path, shadow rows (2026-10-08, README row 37)

Upper bound first (delete the row check and pack, fill with a constant): 85-char lines with scrollback 870 -> 990 MB/s, 170-char 1130 -> 1380 MB/s. The real change beat the bound because the check is gone and no cell is read at all.

| Case | Before | After |
|---|---|---|
| Gate, 10,000-line scrollback | about 790 MB/s | 875-930 MB/s |
| 85-char lines, scrollback | 870 MB/s | 1030 MB/s |
| 170-char lines, scrollback | 1130 MB/s | 1400 MB/s |
| 20-char lines, scrollback | 385 MB/s | 385 MB/s (below 32 cells the old path is kept) |
| Gate, no scrollback | about 1580 MB/s | 1490-1530 MB/s (-4 to -6%) |
| 85 / 170-char lines, no scrollback | 1700 / 2040 MB/s | 1640 / 1920 MB/s (-3 / -6%) |

The no-scrollback loss is real in interleaved A/B runs and survived specializing `put_ascii` on a compile-time `track` flag, so it is code layout or register pressure in the shared loop, not the shadow work (which is skipped there). The real app always has scrollback on the main screen, so the trade is kept; to be revisited if alt-screen output (vim, less) is found to matter.
Safety: a row is marked only when it was blank at column 0 and every later write was `put_ascii` with the same style and no gap; every other writer already sets `dirty = 1` for rendering, which clears the mark. Tests: `make test-shadow` (300 random streams, shadow vs `-DVT_NO_SHADOW`, identical; mutating the style check or the gap check is detected), test-vt 120/0, test-lz 1005/0, test_fast_paths 34163/0, TSan 120/0.
Not measured: GUI benchmark, input latency, 1024x768 (parse engine only).

## Gap 3: lxcat pipe path, splice (2026-10-08, README row 38)

Method: 256 MB ASCII log, `/bin/cat file | lxcat -` typed into a real Lestrix window on a private Xvfb display (software GL, so absolute times are only comparable with each other), terminal answers with a cursor-position report, 7-9 repetitions, old and new `lxcat` binaries alternated. File path floor (`lxcat file`, no pipe): 0.26-0.38 s, median about 0.30 s.

| Case | Before (read + write) | After (splice) |
|---|---|---|
| End to end, run A (median of 9) | 0.604 s | 0.578 s |
| End to end, run B (median of 9) | 0.570 s | 0.519 s |
| Stage alone: pipe -> new tmpfs file per chunk, no parser (C micro-benchmark, 3 runs) | 0.22 s | 0.17 s |
| Stage alone, files overwritten in place instead of created and unlinked per chunk | - | 0.11 s |

Result: about -4 to -9% end to end; kept because the stage micro-benchmark and the interleaved runs agree on the direction. The pipe path (0.52-0.58 s) is still well above the file floor (0.30 s): the stages do not overlap as well as they should. The next lever is the hand-off, see gap 4 (poll sleeps) and the in-place file idea above, which needs an acknowledgement channel from the terminal instead of "the file disappeared".
Rejected by reasoning, not measured: `mmap` of the file in the terminal (saves the 1 MB `pread` copy, at most about 8% of the 0.30 s parse, but a truncated or rewritten file raises SIGBUS inside the terminal).
Tests: `make test-lxcat` (new): empty, 6 B, 9 MB, 40 MB through the pipe (splice) and redirect (fallback) paths, byte-exact; no leftover `/dev/shm/lxcat-*`.

## Gap 4: lxcat hand-off polling, inotify (2026-10-08, rejected)

Change tried: replace the `access()` + 0.5 ms `nanosleep` loops that wait for the terminal to delete the previous chunk file with an `inotify` wait (`IN_ATTRIB | IN_DELETE_SELF`, 100 ms poll fallback). Same 256 MB pipe benchmark as gap 3, interleaved, median of 9: inotify 0.617 / 0.563 s against sleep polling 0.529 / 0.570 s. No win, so it was reverted (the rebuilt binary is byte-identical to the splice build measured in gap 3).
Why it could not help: a timing build of `lxcat` (not kept) showed the pipe path is limited by the terminal, not by the hand-off. Per 256 MB run, 49-60 chunks of about 4.5 MB: `lxcat` spent 0.25-0.46 s waiting for a free slot (the terminal is still parsing the other chunk), 0.22-0.33 s in `splice` (mostly waiting for `cat` to produce), and 0.01-0.04 s sending requests.
New finding: the terminal consumes the same bytes at about 0.5 s per 256 MB when they arrive as pipe chunks, against about 0.30 s when it reads one file. That is the real remaining gap (about 0.2 s) and it lives on the terminal side of `stream_file` / `feed_output` (per-request open, `fstat`, 1 MB `malloc`, `LOCK`, `vt_set_stream_nl` toggles, UI wake-ups on every slice, CPU shared with `cat` and `lxcat` on 6 cores). Next experiment: profile the parser thread during the pipe case (callgrind on `lestrix` is too slow; use a per-phase timer inside `stream_file`).

## Gap 5: `sched_yield` spin in `feed_sliced` (2026-10-08, rejected)

The parser yields up to 2000 times per 64 KB slice while the UI thread waits for the terminal lock. Built three variants with a compile-time limit (`-DYIELD_SPINS=` 2000 / 200 / 0) and ran the lxcat file flood (256 MB, real window on Xvfb, medians of 9, three rounds interleaved): 2000: 0.339 / 0.347 / 0.350 s; 0: 0.353 / 0.365 / 0.337 s; 200: 0.381 / 0.323 / 0.318 s. The spread inside one variant (0.32-0.38) is larger than any difference between variants, so there is nothing to keep; source reverted to the original. A plain `cat` flood is kernel-bound under this harness (8.4 s for 256 MB in every variant, 4.3 s terminal CPU), so it cannot show the effect.
Not measured: a real GPU, where the UI thread holds the lock for a shorter time than under software GL.

## Gap 6: input queue `memmove` (2026-10-08, README row 39)

`flush_out` did `memmove(outq, outq + n, outlen - n)` after every partial write, so a queue of N bytes drained in 4 KB writes costs N^2 / 8 KB of copying. Fix: `outhead` offset, compact only when the buffer needs room (`tcore_send`), reset when empty.
Micro-benchmark of the queue logic alone (C program, non-blocking pipe sized 4 KB, a thread draining 4 KB reads): 8 MB: memmove 0.420 s, head offset 0.073 s; head offset 64 MB: 0.320 s (the memmove version scales with the square of the size; measured only at 8 MB). Label: micro-benchmark plus end-to-end smoke test, not a GUI paste measurement (the SDL script can only inject 31 bytes per text event).
Correctness: a command typed into a real window (Xvfb, `--script "t:touch typed_ok;k:return"`) still runs and creates the file; `make test-gui` passes.
Not measured: real paste of megabytes through the GUI, typing echo latency (unchanged code path for small input: the queue holds a few bytes).

## Remaining gaps: 2, 7, 8, 9, 10 and the stream consumer lead (2026-10-08, README row 40)

Harness fix first: fast output only starts after Enter is pressed in the window (`tcore_cmd_begin`), so `-e script` runs never exercised it. The working recipe is a real window on Xvfb with a typed command: `build/lestrix --local --no-fastcat --working-directory DIR --script "w:2;t:COMMAND||touch d;k:return;w:40"` (text events are 31 bytes and `;` splits script steps), time from launch to the mtime of `d`.

**Gap 2, widen the fast-output list (done, row 40).** Typed `/bin/cat` of 256 MB: 9.1 s with `--no-fast-output`, 2.7-3.3 s with it (the harness works). Added 43 names (list in `fo_comm_ok`). `paste l.txt` (a line-buffered printer, 0.65 s to /dev/null): 24.2 / 24.2 / 24.7 s with the old list against 17.8 / 18.2 / 19.0 s with the new one, interleaved. `diff` of the same file against /dev/null was inconclusive (22 s either way, `diff` itself dominates). Risk accepted: a listed program that later goes raw is handled by the existing termios check (`fo.suspect`), same as `cat file | less` today; `git`, `journalctl`, `systemctl` and anything that starts a pager are deliberately not listed.
Even with fast output, a line-at-a-time writer reaches only about 14 MB/s (paste: 256 MB in 18 s) against 85 MB/s for `cat` (large writes): the per-write wake-ups and sub-1 KB reads are the limit now (gap 9).

**Gap 7, read delay (kept at 200 us).** Sweep on the `paste` flood, medians of two runs: 0 us 30.1 s, 50 us 20.8, 100 us 18.9, 200 us 18.1, 400 us 18.2, 1000 us 21.3. 200 us sits on the plateau. Typing latency: the delay is only slept after a read of under 1 KB and delays the next read, never the one already delivered; worst case 0.2 ms added to an echo that is then waiting for the next frame (16.7 ms at 60 fps). Not measured: key-to-pixel latency (needs a GPU session and a camera or compositor timestamps).

**Gap 8, lock hold per slice (no change).** A 64 KB slice parses in about 45 us at 1.5 GB/s and a 512 KB streamed slice in about 0.35 ms, both far below one frame (16.7 ms); the lock is not what limits the UI. Static reasoning from the measured throughput, not a lock-hold trace.

**Gap 9, read-size histogram (finding).** Python reading a real pty, default kernel path, `read(fd, 65536)`: `ls -R /usr/lib` 24,229 reads, average 114 B; `cat` of 64 MB 106,048 reads, average 612 B (90% under 1 KB, 7 reads over 16 KB); `seq 1 2000000` average 465 B; `find /usr` average 252 B. So real pty traffic arrives in sub-kilobyte pieces: the serial parser path (this round's rows 36-37) and the 200 us coalescing delay matter more than the 128 KB bulk path, which only helps `lxcat`/streamed input.

**Stream consumer lead (no fix).** Timing build (not kept): per 256 MB, `stream_file` total 0.48 s for pipe chunks against 0.30 s for one file; the extra 0.1 s is in `feed_sliced` (0.39 vs 0.26 s) plus 0.04 s of per-request unlink and free. Reusing the 1 MB read buffer across requests (instead of `malloc`/`free` each time) measured 0.604 vs 0.582 s and 0.591 vs 0.601 s, inside noise: reverted. `--parse-threads 1` did not close the gap (feed 0.41 s vs 0.29 s), so it is not worker contention alone. Left open.

**Gap 10, render side (not measured).** Software GL under Xvfb cannot say anything about GPU drawing cost, and the dirty-row cache is already in place (rows 2-8 of the progression). Carried to the Fleetwm test list instead, where a GPU session exists.

## Idle wake-ups and `--lite` (2026-10-08, measured under nested Fleetwm on a KVM VM, 2 vCPU i5-8500, headless/pixman, no governor)

Question: should `--lite` use fewer resources than the normal window at the same throughput? Flood (`seq 1 4000000`, about 30 MB, 8 interleaved runs each): normal and lite are the same within noise (Lestrix CPU median about 326 vs 310 ms, wall 6.3-7.2 s both), because the parse and render engine does the work in both modes. The difference is at idle, and the causes were shared by both modes:
- The idle loop never slept longer than 1 s (`wait = ... left > 1.0 ? 1000`), and every second it stat()ed four Fleetwm theme files to follow the theme. strace of an idle `--lite` window: three `poll` calls and four `stat` calls per second with nothing to do.
- Fix 1: Fleetwm theme changes arrive through an inotify thread on the Fleetwm config directories (`fleetwm_watch_start`), which calls `app_wake()`; the timer check stays as a 30 s safety net (1 s if inotify is unavailable). A change shows up within 1 ms of the file write (strace).
- Fix 2: the idle sleep goes to the next timer, capped at 5 s instead of 1 s.
- Idle, 4 interleaved runs of 30 s each: `--lite` 1.43 -> 0.43 wake-ups/s, 4 -> 1-2 ms CPU per 30 s; normal 2.0 -> about 1.2 wake-ups/s, 9-10 -> 8-9 ms. The remaining normal-window cost is the stats bar redraw at every stats tick (software GL in the VM; not measured on a GPU).
- Flood after the change (6 interleaved runs per variant): normal 6.40-6.82 s (before 6.22-6.77), lite 6.28-6.63 s (before 6.91-7.18): no regression; compositor CPU the same or lower.
- Checks: `make test` and `xvfb-run -a make test-gui` pass.
- Not done: the stats text is still rebuilt every tick (five `/proc` reads); the tick could back off when nothing changed.

## Profile and baseline of 0.4.1, no code change (2026-10-27, i5-8500 2 cores, Xvfb 1024x768 + llvmpipe, GCC 14.2)

Commands (scratchpad scripts, not in the repo): `run.sh BIN {empty|seq|log|redraw|colour} FILE` runs `lestrix --local --flood FILE` and prints flood time, user+sys CPU from `times`, wall; `run.sh BIN idle 20` prints CPU, wake-ups/s, RSS, PSS after 6 s. Files from `bench/gen_workloads.py`. `make test` passed before any measurement.

| Workload (5 runs) | flood time | CPU user+sys |
|---|---|---|
| empty file (start-up only) | 0.01 s | 0.17-0.185 s |
| seq, 31 MB | 0.17-0.25 s | 0.34-0.37 s |
| log, 107 MB | 0.15-0.20 s | 0.31-0.41 s |
| redraw, 14 MB | 0.05-0.06 s | 0.22-0.23 s |
| colour, 40 MB | 0.15-0.22 s | 0.30-0.39 s |

Idle, 20 s, 3 runs: 6.2-7.3 ms CPU, 9.2-9.6 wake-ups/s (half of them llvmpipe threads, software GL only), PSS 69.4 MB (anon 45, file 22, shmem 2), RSS 135 MB. In the main thread: one 3 s idle-bar tick (6 failed theme `stat`s, 5 `/proc` reads, one frame). Engine bench: 1502 / 899 / 743 / 380 MB/s (no scrollback / 10,000 lines / unlimited / colour).

Callgrind, `--run "seq 1 1500000"`, `--separate-threads=yes`, 4.58 G Ir total. 2.9 G is llvmpipe JIT (not ours). Main thread 734 M: dynamic relocation of 105 libraries 108 M (gallium, LLVM; driver-bound), `write_ppm` of the `--shot` 127 M (harness artifact). Parse thread 950 M for 1.5 M lines, about 630 Ir per 8-byte line: `feed_serial` 202, `hist_add` 119, `hist_push` 57, `clear_row` 57, `pack_oldest` 55, `region_up` 53, `do_linefeed` 35, memmove 35. Short rows (< 32 cells) take the generic `hist_add` path; compact bookkeeping for them was already measured at -20% and rejected (round of 2026-10-08).

Result: no new hot spot outside the already-tuned serial path and the driver start-up, so nothing was changed and no speed claim is made. Not measured: real-GPU frame cost, region_up as a ring offset (candidate, at most 8% of the parse thread's instructions, memory-bound so likely less).

## region_up row-order window (2026-10-27, same machine)

Change: a full-screen scroll slides a window over a 2*rows array (`Scr.ordbuf`) instead of moving the row-order entries; the window is moved back once per screenful. Partial scroll regions keep the old loop.

- Engine bench (`tests/bench_vt`, 17 interleaved runs, medians, MB/s): no scrollback 1492 -> 1601 (+7%), 10,000-line scrollback 892 -> 936 (+5%), colour-heavy 383 -> 387 (no regression; the earlier 369 was noise, spread 277-417 on both). An earlier 9-run set gave 1480 -> 1563 and 841 -> 951.
- GUI flood (Xvfb + llvmpipe, 5 interleaved runs, flood time median / CPU median): seq 0.19 -> 0.22 s / 0.37 -> 0.38 s (ranges 0.18-0.23 and 0.20-0.23, overlapping), log 0.14 -> 0.14 s / 0.30 -> 0.31 s, colour 0.12 -> 0.12 s / 0.32 -> 0.31 s. No end-to-end gain; the parse thread is not the limit of the GUI flood. The redraw workload did not finish in the harness (both binaries), so it is not measured.
- Checks: full `make test` passes; the pyte differential gives the same 57 mismatching seeds (of 1500) on the 0.4.1 baseline and on the change, so they are pyte differences, not this change. Mutation check: forgetting to move the window back and an off-by-one in the 2*rows bound fail `test-vt`; using the window for a region one row shorter than the screen was NOT caught, so `test_scroll_window_slide` was added to `tests/test_vt.cpp` and now fails on that mutation.
- Result: a measured engine gain, no measured GUI gain, so no speed claim for the app. Keep or reject is the owner's decision; no README progression row until then.

## Remaining candidates after the 2026-10-27 round (none measured, none started)

- Idle stats tick: back off when nothing changed (stats text is rebuilt every 3 s from five `/proc` reads and six failed theme `stat`s). Main thread uses 4.7 ms CPU per 20 s, so the gain is small.
- GUI flood is not parse-bound (engine +7% gave no flood change): the next lever is the render and upload path or the main thread, not `vt.cpp`. Needs a profile of the flood on the main and llvmpipe threads first.
- Redraw workload: fix the harness (it never finishes on either binary) before judging it.
- Real-GPU frame cost: all numbers are Xvfb + llvmpipe; unmeasured on hardware GL.
- Closed, do not retry without new evidence: compact bookkeeping for short rows (-20%, 2026-10-08), `-fno-plt`, `-march=native`, LTO, PGO, BOLT.

## Redraw harness and render-path profile (2026-10-27, no code change)

- Redraw flood "never finishes": cause found. `bench/gen_workloads.py redraw` ends the file mid-row with no newline, so the harness's `echo FLOOD_DONE_42` lands on the last screen row and is overwritten or wrapped, and `--flood` never sees the marker. Fix for the harness only: append `\r\n` to a copy of the file (the generator is left alone because the file bytes are the shared input of the terminal comparison). With the copy: redraw 14 MB, 7 interleaved runs, flood time median 0.04 s (base, range 0.04-0.06) vs 0.05 s (new, 0.05-0.07), CPU 0.23 vs 0.21 s: no difference between 0.4.1 and the region_up window change.
- Render-path profile: `perf` is blocked here (`perf_event_paranoid` = 3, lowering it needs root). Callgrind (`--separate-threads=yes`, `seq 1 1500000`, `--shot`) gave 831 M Ir in total: main thread 577 M, of which dynamic linking 86 M, `memset` 45 M and `fwrite` 66 M (the `--shot` PPM write, a harness artifact), D-Bus and fontconfig start-up about 60 M; a font-config helper thread 140 M; the parse thread only 72 M (`feed_serial` 23 M, `hist_add` 14 M); llvmpipe 42 M. No render or upload function reaches the top of any thread. Caveat: valgrind slows every thread, so far fewer frames are drawn than in a real flood, so this shows where the instructions go in start-up and parsing, not the per-frame cost of a real flood.
- Result: no hot spot found in the render path with the tools available; the 31 MB flood takes 0.19 s end to end (about 160 MB/s) against 1600 MB/s for the engine alone, and the profile does not say where the rest goes. Needs a wall-clock view (per-thread timestamps or `strace -c -f -T` of the pty read and swap calls), not an instruction count. Not done.
- `strace -f -c` of the seq flood (0.44 s under strace): futex 63% of syscall time (1173 calls, threads blocked waiting), wait4 11%, poll 3.7%, write 3.7%, read 2%. These are blocking times summed over all threads, so they show idle waiting, not cost; no syscall stands out as work. Per-thread timestamps (frame start, upload, swap, parse hand-off) would be needed to find where the 0.19 s goes. Not done.

## Where the flood time goes: LX_TRACE (2026-10-27)

Tool: `src/trace.h`, `LX_TRACE=1 lestrix --local --flood FILE` records timestamped events from all threads (send, native-cat job, feed slices, pty reads, loop wake, frame begin/built/r_end/swapped, marker seen) and prints them sorted to stderr when the flood finishes. Off by default: one relaxed load and a branch per call site. Cost check, seq flood, 6 interleaved runs: without the tracer 0.20 s (0.18-0.33) / CPU 0.35 s, tracer built in but off 0.20 s (0.18-0.21) / 0.36 s: no difference. `make test` passes.

seq flood, 31 MB (about 3.9 M lines of 8 bytes), one traced run, ms after the command was sent:

| step | at | what |
|---|---|---|
| command sent | 0 | `cat FILE; echo ...` typed into the pty |
| native-cat job starts | 50 | shell has forked cat, the probe found it |
| native-cat request | 78 | shell and cat stopped, cat's file offset moved to the end, cat gone, `OSC 7777` sent |
| first feed slice | 78 | reader thread starts feeding 512 KB slices |
| last feed slice done | 241 | 58 slices, 154.5 ms of it inside `vt_feed` |
| marker seen | 241.5 | 0.08 ms after the last slice |

- Parse-bound, not render-bound: 155 of the 163 ms of feeding is the engine itself. 31 MB in 155 ms is 200 MB/s because these are 8-byte lines (about 630 instructions per line); the 1600 MB/s of the engine bench is for 70-byte lines. The two numbers were never comparable, so the "160 vs 1600 MB/s gap" was not a gap.
- Fixed take-over cost: 78 ms from sending the command to the first byte fed (50 ms until the job starts, 28 ms for the stop, offset move and exit of cat). It is paid once per `cat`, so it matters for small files and not for large ones.
- Frames during the flood: 12 swaps; build 0.2-27 ms (27 ms is the first frame, glyph atlas warm-up), mid-flood 0.2-4 ms plus 1.5-7 ms in `r_end` and swap; they overlap the feeding on other threads and do not delay the marker.
- Open (not measured): what the 50 ms before the job starts is (shell fork/exec of cat versus the 3-frames-per-loop probe interval), and the line-rate cost `hist_add` + `hist_push` + `pack_oldest` + `clear_row` for short lines, already the subject of rejected work (2026-10-08).

## LP_NUM_THREADS and the cat take-over, measured (2026-10-27, 2 cores)

- `LP_NUM_THREADS` (llvmpipe worker threads; default = 2 here), 5 interleaved runs per cell, flood time median / CPU median: seq 0.23 / 0.37 s (default), 0.19 / 0.37 (1 thread), 0.20 / 0.38 (2 threads); colour 0.16 / 0.33, 0.14 / 0.31, 0.14 / 0.32; redraw 0.05 / 0.21, 0.05 / 0.20, 0.05 / 0.22. Ranges overlap in every row (the default's range is the widest, 0.19-0.25 on seq), so no measured win: rejected, nothing changed. Not measured: idle CPU and wake-ups with a single thread.
- Take-over, 3 traced runs, ms after the command was sent: foreground job seen by the probe 9.5-15.4 (the shell forks and execs cat and cat's first 4096+ byte write arrives), shell and cat stopped within 1 ms, then 11-30 ms until cat is gone (after SIGCONT it finishes writing its in-flight chunk through the pty and exits), request sent 27-40 ms, marker 210-260 ms. The earlier "50 ms until the job starts" was inflated by the busy first frames of a traced run; the real fixed cost is 27-40 ms, of which the stop dance is under 1 ms (the 0.5 ms `nanosleep` polling in `nc_wait_stopped` is not a cost). Nothing in it can be cut safely: the first part is the shell's own fork and exec, the second is cat draining data it already read. Flood time is 80% feeding the engine.

## Round 2026-11-03: flags, allocator, colour emoji

Engine benchmark (`BENCH_MB=8`, 2 cores, i5-8500, GCC 14.2, 9 interleaved runs, medians; rows: no scrollback / 10k scrollback / unlimited / colour):

- Flag bundle `-fno-math-errno -fno-semantic-interposition -fno-stack-protector -fno-asynchronous-unwind-tables -Wl,-O1,--as-needed,--hash-style=gnu,-z,pack-relative-relocs`: 1657 vs 1645, 935 vs 935, 780 vs 604, 434 vs 409 MB/s. Rows 1-2 identical; rows 3-4 move but their base ranges (401-924, 381-447) contain the result. Rejected, not kept (no consistent win).
- `-fno-trapping-math -fno-exceptions -fno-threadsafe-statics -falign-functions=32`: inside noise on every row. Rejected.
- jemalloc via `LD_PRELOAD`: 1628 / 864 / 658 / 387 against base 1627 / 889 / 556 / 398. Rejected.
- Unlimited-scrollback row is bimodal (about 430 or 880 MB/s) run to run. With the app's own `mallopt` values (arena 2, mmap 64 KB, trim 256 KB) median 613; trim 16 MB 643; trim 64 MB 747; ranges 443-894 overlap. Raising the trim threshold costs idle RSS for no consistent gain: rejected.
- Closed list unchanged: `-fno-plt`, `-march=native`, LTO, PGO, BOLT, `-O2`.

Colour emoji (feature, not a speed change): colour bitmap fonts (CBDT, Noto Color Emoji) were rejected by `open_face` as not scalable. Now the strike nearest the font size is selected, box-scaled to the cell height (at most two cells wide) and uploaded to a second RGBA atlas (1024 x 1024, created on the first emoji glyph, not on ES2). Instances carry a colour flag in `pad` byte 2; the fragment shader branches on it before the mono path. Code points from U+1F000 prefer Noto Color Emoji; BMP symbols (U+2764 etc.) keep text presentation. Without a colour atlas (ES2) the glyph degrades to alpha x luma coverage tinted by the text colour. Non-emoji text only gains one flat varying and one predictable branch; the glyph cache hit path is untouched. Test: `make test-font`. Not measured: frame cost on a real GPU.

### Cache levels (2026-11-03)

i5-8500 here: L1d 32 KB per core, L2 4 MB per core, L3 16 MB shared (lscpu; `perf_event_paranoid` is 3, so no hardware counters; numbers are cachegrind/callgrind simulation with 32 KB 8-way L1 and 8 MB LL, 1 MB benchmark input).

- Engine total: D1 miss rate 2.5% (reads 2.0%, writes 3.1%), last-level miss rate 0.3%. The parser is already cache-resident; there is no large L1 gap to close.
- Largest simulated write-miss sources: `clear_row` memset 340K of 650K D1 write misses (cold row memory, inherent to recycling rows), `sd_lz_compress` read misses 190K (streaming the input once, inherent).
- Tried: shrinking the LZ hash table (zeroed on every call, 32 KB = the whole L1d) from 13 to 12, 11, 10 bits: compressor 9.9 / 10.0 / 10.1 / 10.1 GB/s per core, packed size identical in the benchmark output. Inside noise: rejected, `lz.cpp` unchanged.
- The unlimited-scrollback row is bimodal because of thread placement, not cache size: pinned to one core (`taskset -c 0`) 390-453 MB/s (parser and compress worker time-share), on both cores 477-884 MB/s. Pinning the worker to a second core in the app was not done (core count varies, other loads share the cores); the open question is whether a soft affinity hint helps on 2-core machines.

## CPU renderer for machines without a GPU (2026-11-03)

Profile first (perf-profile, `--fp`, 499 Hz, VM with no GPU, `seq 1 3000000` in a tab, recordings in the claude-memory-bank repo, `perf-data/lestrix-2026-11-03/recordings/`, opened in Hotspot): llvmpipe-0 33% + llvmpipe-1 32% (of which 53% JIT shader code, 13% libgallium), `seq` 20%, Lestrix's own threads 15%, own code about 3%. Kernel symbols resolved after `kptr_restrict=0`. Conclusion: on a GPU-less VM the frame cost is software OpenGL, not the engine.

Change: `src/ui/soft.cpp` rasterises the same RInst stream on the CPU (rects, SDF rounded/bordered rects, mono glyphs from an in-memory R8 atlas, colour glyphs from an RGBA atlas, scissor, `r_clip`); `atlas.cpp` keeps CPU copies of the atlases when `sd_soft`; `render.cpp` skips shaders and GL state; `app.cpp` creates a plain window (SDL framebuffer acceleration hint off, otherwise SDL presents the surface through an OpenGL texture and llvmpipe is back: seen in the first measurement, CPU 1380 ms before the hint, 2000+ ms after it with a 16-bit surface rejected and GL used again, 680 ms with the 32-bit back buffer). The X display here is 16-bit (SDL window surface R mask f800), so frames are drawn into a 32-bit back buffer and SDL converts on `SDL_BlitSurface`.

Selection: `--render auto|cpu|gpu` / `LESTRIX_RENDER`; auto = CPU when there is no readable `/dev/dri/renderD*` and no `/dev/nvidiactl`; GL init failure falls back to CPU.

Results (`flood_bench.sh`: lestrix in a window, `sleep 1.5; seq 1 3000000; touch done`; wall time from 1.4 s after start to `done`, lestrix utime+stime in that span; 5 interleaved runs):

| | wall s | Lestrix CPU ms |
|---|---|---|
| OpenGL (llvmpipe) | 3.80 3.43 3.60 3.27 3.47 (median 3.47) | 2270 2050 2190 1990 2150 (median 2150) |
| CPU renderer | 2.21 2.31 2.01 2.25 2.33 (median 2.25) | 680 730 600 710 740 (median 710) |

Also: first frame 83 ms vs 180 ms, RSS 33 vs 137 MB, PSS 13 vs 74 MB, idle 10 vs 40 ms CPU per 6 s. Profile of the CPU renderer on the same flood: `seq` 44%, Lestrix main thread 39% (`soft_draw` 6%, `soft_clear` 3.8%, rest syscalls and X), `lx-io` 17%; the rasteriser is about a tenth of all samples, so the generator and the pty now dominate.

Open: damage tracking (the whole frame is cleared and redrawn, and `SDL_UpdateWindowSurface` copies it all; a changed-rows rectangle list would cut `soft_clear`, the conversion blit and the X upload), row-parallel rasterising for 4K windows, Wayland and 24-bit X not measured, no real-GPU comparison.

### CPU renderer: changed-tiles repaint (2026-11-03, later)

Profile before: with one line rewritten in place (awk printing `\rcount N` 3 million times) the whole frame was cleared and redrawn every time, `soft_draw` 3.1% and `soft_clear` 2.2% of all samples plus the SDL conversion and upload of the whole window (about 12% in the flood profile). Change (`render.cpp`, `SoftState`): the frame is kept as an instance list (drawing deferred to `r_end`), the window is cut into 32x32 tiles, every instance mixes a hash of itself and its clip into the tiles it touches in draw order, and only tiles whose hash differs from the last frame are cleared and repainted (merged into runs per tile row, instances binned per row). The pixels of the previous frame persist in the surface (or the 32-bit back buffer). Full repaint is forced on first frame, size or surface change, new clear colour, atlas generation change and window expose/resize events. `app.cpp` presents only the repainted rectangles (`SDL_UpdateWindowSurfaceRects`; the back-buffer blit is restricted to them) and presents nothing for an unchanged frame.

Correctness: `LESTRIX_SOFT_VERIFY=1` repaints every frame from scratch into a scratch buffer and compares it with the incrementally painted frame (`fatal` exits 3; `make test-gui` runs the CPU renderer with it). Zero mismatches over the GUI scripts and three floods (`seq`, in-place status line, `ls -la` dump); a mutated build that never repaints reports 22 mismatches, so the check bites.

Results (A/B, `bench.sh` in the claude-memory-bank repo, `perf-data/lestrix-2026-11-03/scripts/`, 5 interleaved runs, CPU renderer both):

| workload | before: wall / Lestrix CPU | after: wall / Lestrix CPU |
|---|---|---|
| in-place status line, 3M updates | 11.27 10.43 10.98 10.57 10.62 s (median 10.62) / 3230 2940 3150 3000 3030 ms (median 3030) | 8.56 9.28 8.72 8.75 8.85 s (median 8.75) / 1480 1570 1530 1490 1580 ms (median 1530) |
| idle, 6 s | 30 ms | 0 ms |
| `seq` flood, 3M lines | median 470 ms CPU (noisy 440-880) | median 540 ms (470-600): no change, every tile of the pane changes each frame |

perf (499 Hz, same workload): Lestrix main thread 1302 samples before, 753 after (-42%); `soft_draw` falls out of the top symbols. Note the first version of the status workload was wrong (a literal `\r` made awk wrap and scroll the whole pane, so 65% of tiles were dirty); the awk program now lives in a file. Open: parallel rasterising for 4K windows, Wayland and 24-bit X untested, real GPU untested.

### UI wake-up storm (2026-11-03, from `status2_after.data`)

Recordings, scripts, binaries and `strace` evidence for this round: `perf-data/lestrix-2026-11-03/` in https://github.com/BeanGreen247/claude-memory-bank (its README says which recording resolves which symbols). Open with `hotspot <file>`.

Profile (perf, `--fp`; flat and with `strace -c -f`): in the in-place status-line workload 63% of the UI thread's samples were inside system calls, 31% of it in syscall entry/exit alone (`syscall_return_via_sysret`, `entry_SYSRETQ_unsafe_stack`, `entry_SYSCALL_64`, plus mitigation work such as `clear_bhb_loop` on this VM). `strace -c` counted 82,269 `recvmsg` calls, 81,451 of them failing with EAGAIN, in 9 s; the stack (`strace -k`) was Xlib's `XEventsQueued` from SDL's event pump. Cause: the reader thread wakes the UI for every 0.2 ms of output (`tcore.cpp` `wake_ui`), up to 5000 wake-ups a second; while a frame was already due the UI thread woke, ran its whole loop (pump, timers, event poll with the X connection check) and went back to sleep until the next wake.

Change (`app.cpp`): `frame_pending` is set while the UI thread sleeps with `a->dirty` set and cleared as soon as it wakes; `app_wake()` returns immediately while it is set. Output is written before `app_wake()` runs and the flag is cleared before the frame is built, so a skipped wake cannot leave output undrawn (the frame, due within one frame gap, re-reads everything).

Results (A/B, `bench.sh`, CPU renderer both, 5 and 6 interleaved runs):

| workload | before | after |
|---|---|---|
| in-place status line, 3M updates | 8.38 8.39 8.72 9.15 8.78 s; Lestrix CPU 1470 1450 1490 1610 1540 ms | 8.08 7.66 8.16 7.86 7.59 s; 440 400 430 410 410 ms |
| `seq` flood, 3M lines | 2.10 2.14 2.01 1.94 1.83 1.06 s; 560 520 510 480 510 360 ms | 2.03 1.16 1.66 0.99 1.41 1.53 s; 300 250 270 250 270 290 ms |
| idle, 6 s | 0 ms | 10 ms (noise) |

perf after: UI thread 13.2% of all samples -> 2.5%; `recvmsg` calls 82,269 -> 8,827 and futex 227 -> 30 in 9 s. Echo latency (strace, echo read on the pty to the next X request from the UI thread, typed `q z x y k` in an idle shell, 4 runs each): before 0.8-12.8 ms, median about 0.9 ms; after 0.4-5.4 ms, median about 0.9 ms (strace overhead included in both): no regression. `make test`, `make test-gui` (both renderers) and `LESTRIX_SOFT_VERIFY` runs on a `seq` and a status-line workload pass.

Left in the profile: `awk` and the pty are now 90% of the status workload; the reader thread does 20k `read` plus 20k `clock_nanosleep` (the 200 us read delay, tuned earlier) in 9 s; SDL's `XSync` after each present (a server round trip per frame) is the remaining UI-thread syscall cost; Xlib polls inside it.

## Read delay 200 -> 400 us (2026-11-05)

Re-sweep with the CPU renderer and `seq`/status workloads (3 interleaved runs each, `--read-delay` N): status-line CPU 0 us 2890-3110 ms, 50 us 660-720, 100 us 470-490, 200 us 370-400, 400 us 300-330, 800 us 240-260; wall time flat from 50 us (7.1-7.8 s, awk-bound); flood CPU flat from 100 us (250-290 ms). Default raised to 400 us (the 2026-10 paste sweep had 200 and 400 on the same plateau, 18.1 s vs 18.2 s; 1000 us was worse at 21.3 s). Release build A/B, 3 interleaved runs: status CPU 380-400 -> 270-300 ms (-27%), wall 7.5 s unchanged, flood CPU 250-270 -> 240-260 ms. The delay is only slept after a read under 1 KB and delays the next read, never the one already delivered. Not measured: key-to-pixel latency. `make test` passes. Raw data: claude-memory-bank `perf-data/lestrix-2026-11-05/`.

## Key-to-pixel latency (2026-11-05)

Method: `k2p.c` sends one `x` with XTest to the focused window and polls `XGetImage` on the echo region until the pixels change (no compositor, so this is the moment the present reached the X server; it excludes the display scan-out and input device time). Idle `sh`, 1440x780 window, 2-vCPU VM, 30 keys per run, only keys sent after 150 ms of stable pixels. Scripts: claude-memory-bank `perf-data/lestrix-2026-11-05/scripts/` (`k2p.c`, `run_k2p.sh`).

CPU renderer, default read delay 400 us: median 0.8-0.9 ms, p90 0.9-1.2 ms, max 2.2 ms (4 runs). OpenGL (llvmpipe): median 9.3-10.2 ms, p90 11-19 ms, max 27.7 ms (two 60 fps frames at worst). Read-delay sweep, CPU renderer, 2 runs each, median: 0 us 0.5, 200 us 0.7, 400 us 0.9, 1000 us 1.5 ms. The delay adds about its own length to an idle echo, so 200 -> 400 us costs 0.2 ms. Not measured: latency while a flood is running, GPU hardware.

Latency under load (same method, CPU renderer, cursor hidden so the cursor jumping to the status row cannot trigger the detector; load writes one status row with save/restore cursor). Idle: median 0.9 ms, p90 1.0. Light load (shell loop, 2 ms between writes): median 0.6-0.7, p90 1.0-1.2, max 2.2. Saturating load (awk, no pacing, one core busy): median 1.2-1.7 ms, p90 9-31 ms, max 35-101 ms, read delay 200 vs 400 us not distinguishable (3 runs of 40 each, p90 10-25 vs 18-31). Probable cause, not verified: the echo queues behind up to a full pty buffer of pending output, plus 2 vCPUs shared by awk, Lestrix, Xorg and the probe. Not fixable by ordering: the echo is data in the same byte stream.

Replay of recorded full-screen sessions (vim with syntax, 300 page-downs, `script` recording x10 = 11 MB; `top -d 0.01`, 19.7 MB; `cat` of the file into Lestrix, 3 runs each): vim 0.26-0.37 s wall, 60-80 ms CPU (CPU renderer) vs 0.42-0.47 s, 140-150 ms (OpenGL); top 0.42-0.47 s, 90-120 ms vs 0.52-0.58 s, 170-200 ms. Profile (`top` replay): `feed_serial` 4.2%, `hist_push` 2.0%, `soft_draw` 1.7%, `csi_dispatch` 1.1% of samples, nothing else of Lestrix above 1%. Startup: window mapped 83-96 ms (CPU renderer), 119-150 ms (OpenGL); startup CPU 50-60 ms vs 160-170 ms; RSS 32 MB vs 133 MB. Memory with scrollback: 3M-line flood (22.9 MB) 38 MB RSS, 300k coloured lines (18.7 MB) 35 MB. The `XSync` per present was not changed: SDL-internal, under 1% of samples in every profile above.

## Colour parsing round (2026-11-09)

Method: a driver that builds 8 MB once and feeds it 30 times into a 176x49 terminal with 10,000 lines of scrollback (best of 30, project flags `-O3 -freorder-blocks-algorithm=simple -fvect-cost-model=unlimited`, perf `--fp` recordings in claude-memory-bank `perf-data/lestrix-2026-11-09/`). The built-in benchmark's colour figure moved 0.27 -> 0.31 s after the feature batch; the driver shows the real parser cost was 424 -> 418 MB/s (1.5%), spread over the parser, no single function. The rest was noise.

| Workload | Before | After |
|---|---|---|
| Colour (`3Xm`, `0m`, `1;9Xm`, `0;3Xm` mix) | 418 MB/s | 444-453 MB/s |
| 256-colour and truecolor (`38;5;N`, `48;5;N`, `38;2;R;G;B`) | 257 MB/s | 307 MB/s |
| Plain log | 960 MB/s | 960-971 MB/s |

Change: `sgr_quick` also takes `0;3Xm`/`1;3Xm`/`1;9Xm`-style pairs and the 256-colour and truecolor forms, so they skip the parameter array and the general SGR loop. `tests/test_fast_paths.cpp` now feeds its oracle one byte at a time (no escape sequence is ever whole in the buffer, so the fast paths cannot run) and compares the resolved fg/bg/attrs/underline colour, not only the cell flags; a deliberately wrong clamp fails on round 0.

Rejected (measured): forcing `put_ascii` out of line (-6%), padded or packed style-cache slots and a 64-bit packed key (no gain, a bad hash made it -7%), `cold` attribute on the graphics-string hooks (+1%), 256 instead of 64 style-cache slots (+1% on a worst-case mix, plain output not better).

Whole application (CPU renderer, `--flood` of a 320 MB colour file through the built-in cat): 0.85 s, 380 MB/s; `feed_serial` 35%, `csi_fast` 12%, `intern_style` 5%, `hist_push` 5%, UI thread about 1%. Idle: 1 ms CPU per 10 s, 7 context switches, 38.6 MB RSS (12 MB anonymous).

Test harness fixes found on the way: `--script` `t:` text was cut at 31 bytes (an SDL text event), so `cd /tmp/...` and long commands in `test-gui` were truncated and the image check could never pass on a machine whose shell starts slowly; it now sends long text in several events. `test.sh` read make's exit status 2 as "skipped", so a failing `test-gui` looked like SKIP.

## Gap hunt round 2 (2026-11-09, project flags, no `-march`, colour driver and a plain-text driver)

Method: driver feeding 32 MB (4 KB chunks) into a 200x50 terminal with 10,000 lines of scrollback, best of 8 interleaved A/B runs. Workloads: colour (first 32 MB of the 320 MB colour file) and plain (random words, 8-24 per line, 32 MB). Recordings in claude-memory-bank `perf-data/scratchpad-2026-11-09/recordings/` (`085047-drv.data` before, `085410-drv_new.data` after the AVX2 change; open with `hotspot <file>`). clang-tidy `performance-*` on `src/vt.cpp`: only three enum base-size notes, nothing to act on. The built-in `bench_vt` profile is dominated by its own workload generator (`printf`, `memmove`), so it was not used for tuning.

Before: colour 281 MB/s; `feed_serial_impl` 28%, `hist_push` 13.5%, `csi_fast` 10%, `memmove` 7.5%, `hist_add` 6.7%, `memset` 6.6%, `pen_commit` 4.8%, `intern_style` 4.3%. `feed_serial_impl` is flat: dozens of source lines, none above 2.5%.

**Applied: AVX2 packing loop in `hist_push`** (`pack_cells_avx2`, chosen at run time with `__builtin_cpu_supports("avx2")`, the SSE2 and scalar loops still take the tail and non-AVX2 CPUs). 16 cells per step: one `vpermd` per 4 cells puts code points in the low half and style words in the high half, one xor/and/or accumulates the "second style or non-ASCII" test, one pack stores 16 bytes. Colour 281-290 to 296-301 MB/s (3-7%, interleaved runs); `hist_push` plus the new function 9.4% of samples (was 13.5%). The shared packing code is now `pack_line()` and uses a scratch buffer inside `Vt` (zeroed once with the struct); a per-call zeroed 512 byte stack buffer cost 7% and was removed. Tests: `./test.sh --no-deps` 10 of 11 groups pass; `engine` fails only on the known 57 pyte seeds (1443/1500 identical, same 57 on unmodified HEAD), `make test-vt` 590 checks 0 failures.

**Dropped: pack straight into the history segment.** `memmove` under `do_linefeed` (8.8%) is the copy of the packed bytes into the segment, so the packer was pointed at the segment to skip it. Allocating the compact buffer early made every cell-only segment carry a spare 26 KB buffer that `pack_oldest` then moved (`memmove` 9.8% to 21.6%, colour -20%). Restricting it to segments that already have a compact buffer and falling back to a stack buffer: colour 0 to -3%, plain -1.5%. Not kept.

**Dropped: `hist_add`.** What is left in it is the cell copy into the history ring (`copy_cells`, 1.4% on the hottest line) and bookkeeping, no avoidable work; nothing changed.

**Dropped: one-entry style cache.** `intern_cached` already has a 64-slot direct-mapped cache in front of `intern_style`. The 4% left in `intern_style` is cache misses of the random 256-colour/truecolor mix, which a one-entry cache cannot serve. Nothing changed.

Not done this round: bloaty (needs a debug-info build), heaptrack, callgrind, PGO/BOLT.

