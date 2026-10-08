#!/bin/bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT

name=$1; shift
out=${LESTRIX_BENCH_OUT:-bench-results}/$name; mkdir -p "$out"
tp=$PPID
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
  if [ -n "$LESTRIX_BENCH_LXCAT" ]; then "$LESTRIX_BENCH_LXCAT" "$file"
  else cat "$file"; fi
  printf '\033[6n'; IFS= read -r -s -d R -t 900 _ </dev/tty
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
  rm -f "$file"
  sleep 1; printf '\033[0m\033[2J\033[H'
done
peak=$(rss VmHWM)
sleep 1; i0=$(cpu); sleep 6; i1=$(cpu)
{ echo "terminal=$(cat /proc/$tp/comm)"; echo "startup_s=$startup"; echo "rss_rest_mb=$rest"; echo "rss_peak_mb=$peak"
  echo "idle_cpu_s=$(printf %.2f "$(echo "$i1 - $i0" | bc -l)")"; echo "done=1"; } > "$out/_meta.txt"
sleep 2
