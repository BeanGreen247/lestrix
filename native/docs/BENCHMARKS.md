# Benchmarks

**Current revision first.** The full benchmark of this revision (all terminals, everyday workloads, refterm's two stress files, random text from 128 MB to 2 GB, CPU, start-up, memory, idle) is in the README under "Current revision: full benchmark, all terminals"; its raw tables are what `native/benchmark` writes to `bench/results/summary.md`. The sections below keep the older measurements and the experiments, labelled with the revision they were taken on, so they do not get mixed up.

Every number here was measured on one machine: Intel i5-8265U (4 cores, 8 threads), Debian 13, X11. The CPU governor was `powersave` (`intel_pstate`, energy preference `balance_performance`, about 3.4 GHz under load) for every table in this document except where stated; with the `performance` governor (about 3.7 GHz) the flood tests ran 7-12% faster and RAM bandwidth was unchanged, see `PERFORMANCE_2026-10-07.md`. The `native/benchmark` summary now records the CPU model and governor on its first line. Nothing is estimated. Where a figure comes from a single run it says so. Lower is better for times, higher for speeds.

## 1. The final benchmark: 1 GiB of random text, `cat` in each terminal

`native/benchmark` runs `bench/run.sh` inside each installed terminal (200x50 window, default settings), which makes each test's file just before the test (random printable ASCII, symbols, tabs and multi-byte UTF-8 in lines of random length; same seed, same bytes every time) and deletes it right after. It also runs the everyday workloads and sizes of 128, 256, 512, 768 MB, 1 GB and 2 GB; results go to `bench/results/` with a `summary.md`. The 1 GiB table below was taken before the 2 GB and size-sweep additions.

| | Lestrix | Alacritty | kitty | xterm | mate-terminal | GNOME Terminal | Xfce Terminal |
|---|---|---|---|---|---|---|---|
| Total sink time | **32,826 ms** | 37,887 ms | 50,231 ms | 112,975 ms | 140,888 ms | 142,355 ms | 147,590 ms |
| Speed | **0.030 GB/s** | 0.026 | 0.020 | 0.009 | 0.007 | 0.007 | 0.007 |
| CPU used | **39.7 s** | 40.4 s | 84.4 s | 112.4 s | 128.9 s | 129.9 s | 134.5 s |
| MB per CPU second | **26** | 25 | 12 | 9 | 8 | 8 | 8 |
| Peak memory | 104 MB | 148 MB | 147 MB | **16 MB** | 76 MB | 68 MB | 67 MB |
| Idle CPU, 6 s afterwards | 0.02 s | **0.00 s** | **0.00 s** | **0.00 s** | 5.30 s* | **0.00 s** | **0.00 s** |

\* mate-terminal shares one server between its windows, so this includes other work.

Lestrix ran this twice before the table was taken: 33,966 ms and 30,729 ms, then 32,826 ms (run-to-run spread about 5%).

### Why nobody can go faster here: the pty

The program's output reaches a terminal through the kernel's pseudo-terminal, and the kernel works on every byte on the way (output processing such as turning `\n` into `\r\n`). A bare Python reader draining a pty as fast as it can, with no terminal at all, gets:

| Reader on the same file | Speed |
|---|---|
| Default terminal settings | 34-40 MB/s (about 31 s per GiB) |
| Newline translation off (`-onlcr`) | 47 MB/s |
| All output processing off (`-opost`) | 211 MB/s |

Programs and shells restore the normal settings, so the first row is the real ceiling for `cat`. Lestrix is within about 5% of it. Reaching 0.5 GB/s through a pty is not possible on this kernel for any terminal.

## 2. The terminal engine alone (no pty, no drawing)

The parser and screen model fed from memory: 64 MB of the same random-text file, 144x33 screen, single thread, best of three.

| Scrollback | Before the single text loop | After |
|---|---|---|
| none | 184 MB/s (0.18 GB/s) | 215-222 MB/s (0.22 GB/s) |
| 10,000 lines | 116 MB/s | 151 MB/s |
| unlimited | 116 MB/s | not re-measured |

Other text, engine alone, 10,000-line scrollback, before and after the malloc tuning and cheaper line copies: `seq` (4M short lines) 37 -> 48 MB/s, log text 200 -> 250 MB/s (`make bench`: 369 -> 407), colour text about +5%, Unicode text 82 -> 86 MB/s. Per scrolled line the history cost went from about 358 to 174 cycles. The big part was glibc trimming and regrowing the heap around every block of 128 line records (10,000 `brk` calls per 30 MB); holding a few MB removed it.

Then scrollback storage changed from one allocation per line to one segment per 128 lines that is handed to the compressor whole (single runs, before -> after): `seq` 40 -> 59 MB/s, log text 200 -> 280 MB/s, colour text 222 -> 232 MB/s, Unicode text 70 -> 72 MB/s; `make bench` log text with 10,000 lines of scrollback 407 -> 462 MB/s. Resident memory for a 10,000-line scrollback grew by about 3 MB (40.9 -> 44.1 MB in the test program). Output is byte-identical to the old storage in a differential run, and a helper thread that overlaps file reading with parsing for `lxcat` gave no measurable gain (2.18 s for 256 MB either way), so it was not kept.

On ordinary log text the same engine does about 900-1,070 MB/s without scrollback and 350-400 MB/s with it (`make perf-gate`). Random text is the worst case: every few characters is a tab, a multi-byte character or a line end, and nothing repeats, so each cell is genuinely different. The change that gave the gain is one loop for ASCII, 2-4 byte UTF-8, tabs, CR and LF (the old loop stopped at each of those), plus a width table built from the general width function. Instructions per byte dropped about 20%; the rest of the time is in memory traffic and scrollback bookkeeping.

## 3. Everyday workloads, eight terminals

Full application in a normal window (Lestrix with its sidebar), default settings, 200x50 windows, `cat` of prepared files. Lestrix numbers are the median of three runs, the others single runs.

| | Lestrix | Alacritty | kitty | mate-terminal | GNOME Terminal | Xfce Terminal | xterm |
|---|---|---|---|---|---|---|---|
| Startup to first prompt | 0.18 s | 0.19 s | 0.96 s | 0.22 s | 0.39 s | 0.34 s | **0.09 s** |
| `cat` 107 MB log | **1.4 s** | 2.5 s | 2.7 s | 3.2 s | 3.3 s | 3.4 s | 6.4 s |
| `cat` 40 MB colour-heavy | **0.42 s** | 0.92 s | 1.08 s | 1.36 s | 1.35 s | 1.43 s | 2.14 s |
| `cat` 30 MB Unicode text | **0.35 s** | 0.90 s | 0.78 s | 1.68 s | 1.84 s | 1.82 s | 1.98 s |
| `seq 1 4000000` | **1.16 s** | 1.29 s | 3.32 s | 2.64 s | 2.59 s | 2.68 s | 5.20 s |
| 1,500 full-screen redraws (11 MB) | **0.06 s** | 0.22 s | 0.23 s | 0.18 s | 0.24 s | 0.30 s | 1.46 s |
| Peak memory after the runs | 106 MB | 148 MB | 146 MB | 67 MB | 68 MB | 66 MB | **16 MB** |
| Idle CPU over 6 s | 0.02 s | **0.00 s** | 0.02 s | **0.00 s** | **0.00 s** | **0.00 s** | **0.00 s** |

The 107 MB log has the same kind of ceiling: a bare pty reader needs about 1.5 s for it, and Lestrix finishes in 1.4-1.6 s.

## 4. Lestrix's parts, measured alone

| Part | Speed |
|---|---|
| Terminal engine, scrollback off, log text | about 870-1,070 MB/s |
| Terminal engine, 10,000-line scrollback, log text | about 100-400 MB/s (varies with the text) |
| Scrollback compressor, per core | about 7 GB/s |
| 118,000 log lines held in scrollback | 2.7 MB (packed 27x) |
| Whole app, 107 MB flood, CPU of all threads | about 2.0 s |

## 4b. Threads

Lestrix runs: one UI thread, one parser thread per tab, helper threads that build big batches of plain text (half the cores, up to 8; `--parse-threads N`, 0 turns it off), threads that compress old scrollback (a quarter of the cores, 2 to 8; `--compress-threads N`), and a few short-lived ones (font loading). Engine alone with 10,000 lines of scrollback, built with `-O3`, 4 parse workers, single runs:

| Text | One thread | Bulk on 4 workers |
|---|---|---|
| random mixed text | 216 MB/s | 282 MB/s |
| Unicode text | 108 MB/s | 225 MB/s |
| `seq` (short lines) | 112 MB/s | 188 MB/s |
| log text (long ASCII lines) | 626 MB/s | 610 MB/s (the ordinary parser is faster here, and it is chosen) |

In the whole app, 256 MB of random text through `lxcat`: 1.52 s with `--parse-threads 0`, 1.26 s with 2, 1.15 s with 4 (the default on this machine), 1.18 s with 6. Compress workers: one cannot keep up and makes the parser compress inline (40-49 MB/s on `seq`); 2 to 4 give 56-74 MB/s and 8 is no better. Split I/O (`--io-threads 2`, on from 4 cores up): a reader thread takes bytes off the pty into a ring of 8 buffers, the parser thread parses, a writer thread sends input. Through `cat`, whole-app times in Lestrix, two runs each, off -> on:

| Workload | `--io-threads 0` | `--io-threads 2` |
|---|---|---|
| Unicode 30 MB | 0.92 / 0.91 s | 0.52 / 0.50 s |
| colour 40 MB | 0.88 / 0.86 s | 0.74 / 0.68 s |
| 1,500 full-screen redraws | 0.44 / 0.42 s | 0.29 / 0.29 s |
| `seq` 4M lines | 1.18 / 1.27 s | 1.21 / 1.18 s |
| log 107 MB | 1.37 / 1.44 s | 1.45 / 1.55 s |
| 128 MB random (pty-bound) | 3.39 / 3.56 s | 3.59 / 3.67 s |

It wins where the parser is the slow part (Unicode, colour, redraws) and loses about 6% where the pty is (plain long lines), at about 35% more CPU for the hand-over. Render helper threads (`--render-threads N`) are available but off: a frame costs 0.52-0.56 ms to build either way, and with 2 helpers the uncapped frame rate went from 960 to 870 fps.

## 5. lxcat (Lestrix only) and the refterm stress files

`lxcat` has the terminal read the file itself instead of sending it through the pty. On this machine (single runs, after the DSR-synchronised timing was added, which waits until the terminal has parsed everything):

| | `cat` in Lestrix | `lxcat` in Lestrix |
|---|---|---|
| 128 MB random text | 3.63 s (0.034 GB/s) | 0.98 s (0.127 GB/s) |
| 256 MB random text | 7.74 s (0.032 GB/s) | 2.11 s (0.119 GB/s) |
| 128 MB `longline` (refterm: random letters, no newline) | 3.44 s | 0.98 s |
| 128 MB `manyline` (refterm: random letters, newline about every 27) | 3.44 s | 1.01 s |

With the pty out of the way the limit is Lestrix's own parser with 10,000 lines of scrollback (about 0.12-0.13 GB/s on random text, see section 2). The `benchmark` script runs it as a separate `lestrix-lxcat` row; the other terminals cannot do this, so it is not a like-for-like comparison.

The refterm tests come from its `splat2` program: `-longline` writes random letters with no newline, `-manyline` adds a newline as a 27th symbol, both in 64 MB writes up to 1 GiB. `benchmark` generates the same kind of file (`--refterm-mb` sets the size, default 1024). refterm's other check, plain UTF-8 files and VT colour files, is covered by the Unicode and colour workloads.

## 6. The reader and the pty

The pty hands the reader about 87 bytes per `read()` on this file (it is one line at a time), so 1 GiB took about 12 million reads. A bare C reader confirms the limit is the writing side: reads of 4 KB, 64 KB and 1 MB all got 36-43 MB/s. Waiting after a small read changes the number of reads a lot and the speed little:

| Bare reader, wait after small read | Speed | Reads for 272 MB |
|---|---|---|
| none | 46 MB/s | 2.9 million |
| 5 us | 39 MB/s | 0.7 million |
| 20 us | 37 MB/s | 0.28 million |
| 100 us | 35 MB/s | 0.07 million |

In Lestrix (`--read-delay US`; this section describes the first version, 20 us after every read under 8 KB; the delay is now 200 us and only after reads under 1 KB, see the README progression row 27), 1 GiB of random text went from 38.3 s CPU to 22.9 s at the same wall time (32.1 s and 32.5 s), and the 107 MB log from 2.2 s CPU to 1.4-1.9 s with the same or better wall time.

## 7. Frame-rate limits

Drawing is capped at 60 fps by default (`--max-fps 60`; 0 removes the cap and vsync) and slows to 24 fps while a big dump is pouring in (`--io-fps 24`; lower favours output speed, 0 adds no extra limit); an idle window draws 0-2 frames a second (blink only while focused, the overlay only when its text changed). The overlay's `fps` figure is frames actually drawn.

Frames drawn during a flood of random text, uncapped (single runs): 940 fps with plain `cat` (34 before the parser stepped aside for the drawing thread), 730 fps with `lxcat` (118 before). The cost of drawing that often shows in the dump speed, 256 MB through `lxcat`:

| `--io-fps` | 60 | 120 | 240 | 500 | 1000 | 0 (no limit) |
|---|---|---|---|---|---|---|
| Time | 1.98 s | 2.06 s | 2.25 s | 3.29 s | 3.15 s | 3.19 s |

Why a low rate during dumps: every frame takes the parser's lock and costs it time, so fewer frames mean a faster dump (24 is the default; 60 is shown above because it was the lowest measured here). For reference, kitty documents one repaint every 10 ms (about 100 fps) as "more than sufficient for most uses" with 3 ms of input coalescing and monitor sync on by default; Ghostty defaults to vsync and warns that disabling it costs CPU and power. The defaults here are 60 normally and 24 during a dump.

Above about 240 fps each frame costs the parser more than it gains. If you want the highest frame rate during dumps rather than the fastest dump, use `--io-fps 0`.

## 8. Where to find the rest

The step-by-step optimisation record, including everything that was tried and rejected, is in the README under "Optimization progression". The C++ port analysis is in `CPP_PORT_RESEARCH.md`.
