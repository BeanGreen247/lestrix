# Would a C++ port make Lestrix faster or easier to maintain?

Measured on one machine (Intel i5-8265U, 8 threads, Debian 13, gcc 14.2, clang 19.1). Every number below comes from a run on
that machine; nothing is quoted from elsewhere.

## Short answer

- **Speed: no, not from the language.** The same engine built as C++ with g++ -O3 ran at the same speed as the C build.
  Every real speed-up found during this research is language-independent and can be done in C.
- **Maintenance: yes, in specific places**, mostly object lifetime and ownership. That is where this project has had its bugs.
- **Recommended path:** keep the terminal engine (`vt`, `lz`) in C, convert the application layer to C++ in stages, and do the
  language-independent speed work first, because it pays off whichever language wins.

## 1. Does C++ make the hot path faster? Measured: no

The terminal engine alone (`BENCH_MB=8`, six runs each, MB/s with scrollback off / 10,000-line scrollback / unlimited / colour):

| Build | scrollback off | 10,000 lines |
|---|---|---|
| gcc -O3, C | 1,047-1,107 | 248-281 |
| g++ -O3, same source as C++20 | 969-1,088 | 250-289 |
| clang -O3, C | 910-1,017 | 235-264 |

The C++ build is statistically identical to the C build, and clang is about 10% slower than gcc on this code. `-march=native`,
LTO, profile-guided optimisation and unrolling were each tested earlier and gave nothing on top of `-O3`.

**What it costs to build the existing engine as C++:**

- The C11 atomics need a different spelling (`std::atomic`); a 6-line shim was enough for g++.
- g++ needs `-fpermissive`; strict C++ (which clang++ enforces) needs about **23 explicit casts** from `void*` in `vt.c` alone.
- `lz.h` and the other headers need `extern "C"` guards if any C object is linked in.

## 2. Where the time actually goes (callgrind, parser thread, 10,000-line scrollback)

| Part | Share of parser time |
|---|---|
| `hist_push` (moving a scrolled line into history), in total | 51% |
| of which packing 128 lines into a compressed block | 25% |
| of which `malloc` / `free` of the per-line records | about 20% |
| of which `memcpy` | about 16% |
| `region_up` (screen scrolling) | 14% |

The no-scrollback path runs at about 1,060 MB/s, and each input byte becomes one 8-byte cell, so it is already moving about
8.5 GB/s of cell writes. That path is limited by memory bandwidth, not by instructions.

## 3. Speed-ups found (all language-independent)

| Idea | Measured result | Status |
|---|---|---|
| Pool the per-line history records by size class instead of `malloc`/`free` | 10,000-line scrollback: about 250 -> about 315 MB/s (+25%) | **applied** in `vt.c` (see section 8) |
| Adaptive read batching: while a stream runs, wait up to 20 us for the pty buffer to reach 1 KB before reading (`FIONREAD`) | standalone with a simulated parser: reads 380,000 -> 20,000, reader CPU 1.25 s -> 0.43 s at unchanged throughput. In the app: about 10% less CPU on `seq`, within noise on the log | applied, then **removed again**: once the parser got faster it cost more than it saved (see section 8) |
| The same idea as a plain 50 us sleep after each read | **lost 2x wall time in the app** (log 1.5 s -> 2.5-3.6 s); only the adaptive version is safe | reverted, kept here as a warning |
| Narrower cells (4 bytes instead of 8) | not measured; the no-scrollback path is bandwidth-bound, so up to about 2x there | idea |
| Pack history straight from the line records instead of copying into a staging buffer first | not measured; removes most of the 16% memcpy share | idea |
| GPU-side cell grid (the refterm approach): upload cells, one shader draws the screen | not measured; the UI thread is now only about 12% of a flood's CPU, so the ceiling is small | idea |
| Per-frame arena for the renderer's temporary row copies | UI thread is about 0.25 s of a 2 s flood, so at most a couple of percent | idea |

Earlier in the project, the same method found: the scrollback compressor 2.7 -> 7 GB/s (8-byte compares, SSE2 transpose), the
renderer no longer holding the terminal lock (parser stalls of up to 45% removed), and a redraw loop that spun while waiting for
the flood gap (about 2x CPU). None of those depended on the language either.

## 4. What C++ would actually buy: maintenance

Bugs hit while building the SDL frontend that ownership types would have prevented or made obvious:

1. **Sidebar panel destroyed on move.** Removing a widget from a container dropped the last reference. `shared_ptr` / a
   handle type makes the lifetime explicit.
2. **Dangling pane pointers in dialogs.** The file browser had to keep a global "live panes" registry so a dialog callback could
   check a raw pointer was still valid. `weak_ptr` is exactly that, built in.
3. **Manual frees of per-tab, per-row and per-dialog data** (`tab_free`, `rows_clear`, `cap_free`, `conn_free`): every one is a
   place to forget a field. RAII members free themselves.
4. **Hand-rolled dialog vtable** (`draw`, `free_fn`, a struct cast per dialog type). `std::variant` or a small base class with a
   virtual destructor is shorter and cannot be cast wrongly.

Not prevented by C++ (design bugs, found by testing): the stale row cache after `hist_push`, the stuck-modifier test harness
bug, the zero-timeout redraw spin.

Places where C++ features fit naturally: `constexpr` lookup tables (wcwidth, UTF-8, box drawing) built at compile time,
`std::span` for the row-source view, templates for the two text-run builders, `std::string_view` in the toolkit's text calls,
`std::pmr` arenas for per-frame scratch.

## 5. What it costs

| Cost | Measured |
|---|---|
| Compile time of a file that includes the usual STL headers | 1.15-1.21 s with g++ -O2, against 0.02 s for a C file doing the same job (about 50x). The whole C project compiles in about 10 s on one core, so expect that to grow several-fold unless templates and headers are kept lean |
| Binary size of a small program | 16 KB (C), 26 KB (C++, shared libstdc++), 295 KB (C++, libstdc++ linked statically) |
| Process start-up, 1,000 runs | 0.52 s (C), 1.12 s (C++ shared libstdc++), 0.54 s (C++ static). Linking libstdc++ statically removes the cost, and Lestrix already starts in about 0.2 s, so this matters |
| Strictness | clang++ rejects what g++ -fpermissive accepts; the Windows toolchain (mingw or MSVC) adds a second compiler to satisfy |
| Dependencies | GLib stays either way; `-fno-exceptions -fno-rtti` keeps the "C with classes" cost model |


## 8. Second round: aggressive engine optimization, measured

How other fast terminals get their speed (read from their own documentation and source listings, not quoted):

- **kitty:** a glyph cache in video memory; child I/O on its own thread, separate from rendering; byte-stream parsing with
  vector CPU instructions; and deliberate small repaint and input delays (`repaint_delay`, `input_delay`) that trade a few
  milliseconds for much less CPU. Its published numbers put it at about twice the next terminal on ASCII and Unicode throughput.
- **foot:** damage tracking with double-buffered frames that pre-apply the previous frame's damage, a profile-guided-optimisation
  build step, and a hand-tuned escape-sequence parser (`csi.h` shows "performance improvements" commits).
- **Alacritty / Ghostty:** GPU glyph atlas with instanced cells (what Lestrix now does); Ghostty additionally keeps terminal
  memory in fixed pages with per-row dirty flags and a style table, much like Lestrix's slot-based rows and interned styles.

What was applied to the Lestrix engine, each measured (engine alone, 24 MB inputs, 10,000-line scrollback, best of five runs on
one pinned core, before = the committed engine):

| Workload | before | after | change |
|---|---|---|---|
| plain log | 108 MB/s | 174 | +61% |
| `seq` (short lines, with the `\r\n` a real pty produces) | 38 | 52 | +37% |
| Unicode (Latin, Greek, Cyrillic, box drawing, CJK) | 68 | 148 | +118% |
| colour (many SGR changes) | 104 | 146 | +40% |
| full-screen redraws | 273 | 290 | +6% |

The changes, in order of value:

1. **Bulk UTF-8 path** (`utf8_run`): runs of valid two- and three-byte sequences whose width is known are written straight into the
   row, skipping the per-character width lookup, wrap test and dirty bookkeeping. It stops at anything it does not handle (a
   sequence cut by the end of the buffer, overlong or surrogate forms, combining marks, emoji, a character that must wrap) and
   hands that character to the general path, so correctness never depends on the fast path being complete.
2. **Pooled history-line records** by size class, instead of `malloc`/`free` for every scrolled line.
3. **Cheap limit pre-check** so the per-line limit enforcement runs only when a limit could actually be crossed.
4. **One-line scroll specialisation** in `region_up`, and the `getenv` in the compression-job path read once.

**Safety gate.** A differential fuzz test (`tests/test_fast_paths.c`, `make test-fast`) feeds the same random text, split at random
points, to two terminals, one with the fast path and one without, and compares every screen cell, the cursor and the whole
history; it also checks every code point the fast path claims a width for against the general width function. It found two
things immediately:

- a **real pre-existing bug**: a double-width character written in the last column left the row's high-water mark one past the
  row, and the next clear wrote off the end of the buffer (heap overflow, found under AddressSanitizer). Fixed: such a
  character now blanks the last cell and wraps, as xterm does.
- two **mistakes of mine**: U+3099 and U+309A are combining marks of width 0, but my fast range claimed width 2. Fixed.

**Tried and rejected.**

- *Read batching on the pty* (wait a few microseconds so each read returns more): impressive offline (reads 380,000 -> 20,000,
  reader CPU 1.25 s -> 0.43 s), neutral to harmful in the app once the parser got faster (colour 0.42 s -> 0.55 s), because a
  5 microsecond sleep really costs about 55 on Linux. Removed. The kernel's pty reads (about 0.7 s of a 107 MB flood) remain
  the largest single cost, and no safe way to shrink them was found.
- *A plain 50 microsecond sleep after each read:* lost 2x wall time. Reverted at once.
- *Compiler-level "extreme" flags* (unrolling, no stack protector, no CET, `-march=x86-64-v3`, clang, LTO, PGO): none beat plain
  `-O3` gcc on this code.

**Where the ceiling is.** The 107 MB log is limited by the kernel: a bare reader doing nothing with the bytes takes about 1.5 s,
and Lestrix finishes in about 1.5-1.6 s. No terminal in any language can beat that figure on this machine.

## 9. Conclusion on the language question

After this round the engine is faster by 40-120% on every parsing workload with no language change, and the one place a C++
rewrite was supposed to help (specialised, template-generated hot paths) is also available in C by writing the specialised
function once. A C++ build of the same code is bit-for-bit as fast (section 1). What remains in favour of C++ is the maintenance
argument in section 4, not speed.

## 10. Third round: flags, bit-level changes and start-up

Sources read through a Chrome driven by Playwright: GCC's optimization-option reference and the Bit Twiddling Hacks index. Every
candidate was measured with **interleaved A/B runs** (baseline and candidate alternating, pinned to one core, best of several), because
a first attempt that ran candidates one after another showed the machine slowing steadily and would have blamed the last flags for it.

| Candidate | Result (geometric mean over five workloads) | Decision |
|---|---|---|
| `-freorder-blocks-algorithm=simple` | +4.8% | kept (with the next one) |
| `-fvect-cost-model=unlimited` | +2.9% | kept; the two together: **+5.4%**, every workload +3% or more |
| `-falign-functions=64 -falign-loops=32 -falign-jumps=32` | +1.3% | not kept |
| `-fipa-pta` | +0.2% | not kept |
| `-fno-stack-protector -fcf-protection=none` | -0.2% | not kept |
| `-fno-plt -fno-semantic-interposition` | -1.1% | not kept |
| Ring-buffer indexes as masks instead of `%` (the history and block rings are now powers of two) | +1% | kept, harmless; integer division was never a real cost here |
| Escape parameters parsed in place + 64-entry style cache | colour +7.6%, redraws +8.3% | kept |
| AVX2 for the ASCII scan and cell fill, chosen at run time | +0.2% | removed: the loops are limited by memory traffic, not instructions |
| Profile-guided optimisation, trained on all five workloads | +1.4%, mixed | not kept |
| Reader thread separate from the parser thread (hand-over queue of 16 buffers) | log 0.1 s **slower**, `seq` slower, Unicode and colour level | reverted: the floods are limited by the pty and the producer, not by parsing |

**Start-up** (`LESTRIX_TIMING=1` prints each phase): to first frame about 185 ms, of which window creation about 90 ms (graphics driver
loading), fonts about 37 ms, GL context 20 ms, atlas and shaders 17 ms. Opening only the regular face of each font up front and
loading bold, italic and bold-italic on first use cut the fonts phase to about 10 ms; fontconfig is warmed on a helper thread while
the window is created; the atlas no longer uploads 4 MB of zeros. Result: start-up 0.23 s -> about 0.18 s, ahead of Alacritty and
mate-terminal.

**What is left.** Window creation (the driver) and the kernel's pty reads are the two big fixed costs; neither can be removed from
inside the program. The remaining engine ideas (narrower cells, packing history without a staging copy) are larger changes with a
modest ceiling.

## 6. Recommended plan, if you decide to go ahead

1. **Do the language-independent speed work first** (pooled history lines, then the staging-copy removal, then cell width), with
   the benchmark suite as the gate. It is the only part that changes the comparison table.
2. **Switch the build to C++20** with `-fno-exceptions -fno-rtti -static-libstdc++ -static-libgcc`. Keep the engine files as
   they are (`extern "C"` boundary), and add the 23 casts only if you want to compile them as C++ too.
3. **Convert leaf modules first:** `dialogs.c` (variant + RAII), `filespane.c` (`weak_ptr` instead of the registry), `store` /
   `xfer` (`std::string`, `std::vector`). Each is independent and testable with the existing scripted-window test.
4. **Convert `app.c` last,** splitting it as it goes (menus, tab strip, sidebar, settings), since it is the largest file.
5. **Keep a gate:** `make test`, `make test-gui`, and the cross-terminal benchmark must not regress at any step.
6. **Windows port** is easier from C++ only if the toolchain choice is made first (mingw-w64 keeps GCC; MSVC would need the
   POSIX calls wrapped either way).

## 7. Limits of this research

One machine, one workload family, a pty-bound ceiling that hides some engine differences, and no Windows measurements. The
pooled-allocator number comes from a scratch copy of the engine, not from the application.
