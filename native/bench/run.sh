#!/bin/bash
# run.sh NAME SPEC... - run INSIDE the terminal under test (it must be this script's parent process); `benchmark` does this.
# SPEC says which test to run: w:NAME (an everyday workload: log, colour, unicode, seq, redraw), r:MB (MB megabytes of random text) or
# t:longline:MB / t:manyline:MB (refterm's stress files). Each test's file is generated just before it runs, in $LESTRIX_BENCH_DIR, and
# deleted right after it, so only one test file ever exists (the generator tools come from $LESTRIX_BENCH_GENR and $LESTRIX_BENCH_GENW).
# For each test it prints the total sink time on screen and records wall time, throughput, CPU and memory. At the end it records start-up,
# resting memory and idle CPU, writes $LESTRIX_BENCH_OUT/NAME/*.txt and exits, which closes the terminal.
name=$1; shift
out=${LESTRIX_BENCH_OUT:-bench-results}/$name; mkdir -p "$out"
tp=$PPID                                    # the terminal process (for VTE terminals this is the terminal server)
cpu() { awk '{r=$0; sub(/.*\) /,"",r); split(r,a," "); printf "%.3f",(a[12]+a[13])/100}' /proc/$tp/stat; }
now() { date +%s.%N; }
rss() { awk -v k="$1" '$1==k":"{printf "%.0f",$2/1024}' /proc/$tp/status; }
startup=""; [ -n "$LESTRIX_BENCH_T0" ] && startup=$(echo "$(now) - $LESTRIX_BENCH_T0" | bc -l | xargs printf %.2f)
sleep 1; rest=$(rss VmRSS)
dir=${LESTRIX_BENCH_DIR:-/tmp/lestrix-bench}; mkdir -p "$dir"
file=$dir/test-$$.txt
trap 'rm -f "$file"' EXIT
for spec in "$@"; do
  rm -f "$file"
  case $spec in
    w:*) label=${spec#w:}; python3 "$LESTRIX_BENCH_GENW" "$dir" "$label" >/dev/null && mv "$dir/$label.txt" "$file" ;;
    r:*) mb=${spec#r:}; label=random-${mb}MB; "$LESTRIX_BENCH_GENR" "$file" "$mb" ;;
    t:*) IFS=: read -r _ label mb <<<"$spec"; "$LESTRIX_BENCH_GENR" "$file" "$mb" 1 "$label" ;;
    *) continue ;;
  esac
  bytes=$(stat -c %s "$file")
  sleep 0.5
  c0=$(cpu); s=$(now)
  if [ -n "$LESTRIX_BENCH_LXCAT" ]; then "$LESTRIX_BENCH_LXCAT" "$file"   # Lestrix's fast path: the terminal reads the file itself
  else cat "$file"; fi
  printf '\033[6n'; IFS= read -r -s -d R -t 900 _ </dev/tty   # ask where the cursor is: the answer only comes once the terminal has parsed everything before it
  e=$(now); c1=$(cpu)
  wall=$(echo "$e - $s" | bc -l); cpus=$(echo "$c1 - $c0" | bc -l)
  ms=$(echo "$wall * 1000" | bc -l | xargs printf %.0f)
  gbs=$(echo "$bytes / 1073741824 / $wall" | bc -l | xargs printf %.3f)
  printf '\033[0m\n=== %s: total sink time %s ms (%.2f s)   speed %s GB/s   terminal CPU %.2f s ===\n' "$label" "$ms" "$wall" "$gbs" "$cpus"
  {
    echo "bytes=$bytes"; echo "wall_ms=$ms"; echo "gb_per_s=$gbs"; echo "cpu_s=$(printf %.2f "$cpus")"
    echo "mb_per_cpu_s=$(echo "$bytes / 1048576 / ($cpus + 0.001)" | bc -l | xargs printf %.0f)"
    echo "rss_mb=$(rss VmRSS)"
  } > "$out/$label.txt"
  rm -f "$file"   # this test's file goes now, before the next one is made
  sleep 1; printf '\033[0m\033[2J\033[H'
done
peak=$(rss VmHWM)
sleep 1; i0=$(cpu); sleep 6; i1=$(cpu)      # idle CPU with the last result on screen
{ echo "terminal=$(cat /proc/$tp/comm)"; echo "startup_s=$startup"; echo "rss_rest_mb=$rest"; echo "rss_peak_mb=$peak"
  echo "idle_cpu_s=$(printf %.2f "$(echo "$i1 - $i0" | bc -l)")"; echo "done=1"; } > "$out/_meta.txt"
sleep 2                                      # leave the last result readable, then exit: the terminal closes with its command
