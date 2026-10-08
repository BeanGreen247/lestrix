#!/bin/bash
# lxflood.sh REPS "BIN_A[:opts]" "BIN_B[:opts]" ... ; interleaved flood runs in nested Fleetwm. variant = path|opts
REPS=$1; shift; P=/home/dev/perf-test
cpu(){ awk '{print int($1/1000000)}' /proc/$1/schedstat; }
for r in $(seq $REPS); do for v in "$@"; do
  bin=${v%%|*}; opts=""; [ "$v" != "$bin" ] && opts=${v#*|}
  pkill -u dev -x fleetwm; pkill -u dev -x lestrix; sleep 1
  bash $P/nest_headless.sh gen >/dev/null 2>&1; sleep 3
  RT=/tmp/ptest-run-gen; export XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland
  CP=$(cat $RT/pid); D=/tmp/lxwork; rm -rf $D; mkdir $D
  s=$(date +%s.%N)
  $bin --working-directory $D $opts -e sh -c "sleep 3; seq 1 4000000; touch d; sleep 2" >/tmp/lx.out 2>&1 & LP=$!
  sleep 1.5; c0=$(cpu $CP); l0=$(cpu $LP)
  for i in $(seq 1 400); do [ -e $D/d ] && break; sleep 0.05; done
  e=$(date +%s.%N); c1=$(cpu $CP); l1=$(cpu $LP)
  echo "$(basename $bin) [$opts] total_s=$(awk -v s=$s -v e=$e 'BEGIN{printf "%.2f",e-s}') comp_ms=$((c1-c0)) lestrix_ms=$((l1-l0)) done=$([ -e $D/d ] && echo y || echo N)"
  kill $LP 2>/dev/null; wait $LP 2>/dev/null; kill $CP; sleep 1
done; done
