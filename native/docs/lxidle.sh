#!/bin/bash
# lxidle.sh REPS SECS "BIN|opts"... : idle Lestrix (one shell tab) in nested Fleetwm; CPU ms and wakeups/s of lestrix and compositor
REPS=$1; T=$2; shift 2; P=/home/dev/perf-test
cpu(){ awk '{print int($1/1000000)}' /proc/$1/schedstat; }
vc(){ awk '/^voluntary_ctxt_switches/{a=$2} /^nonvoluntary/{b=$2} END{print a+b}' /proc/$1/status; }
for r in $(seq $REPS); do for v in "$@"; do
  bin=${v%%|*}; opts=""; [ "$v" != "$bin" ] && opts=${v#*|}
  pkill -u dev -x fleetwm; pkill -u dev -x lestrix; sleep 1
  bash $P/nest_headless.sh gen >/dev/null 2>&1; sleep 3
  RT=/tmp/ptest-run-gen; export XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland
  CP=$(cat $RT/pid)
  $bin --working-directory /tmp $opts -e sh -c "sleep 200" >/dev/null 2>&1 & LP=$!
  sleep 8
  c0=$(cpu $CP); l0=$(cpu $LP); w0=$(vc $LP); k0=$(vc $CP); sleep $T
  c1=$(cpu $CP); l1=$(cpu $LP); w1=$(vc $LP); k1=$(vc $CP)
  echo "$(basename $bin) [$opts] lestrix_ms=$((l1-l0)) lestrix_wake/s=$(awk -v a=$w0 -v b=$w1 -v t=$T 'BEGIN{printf "%.2f",(b-a)/t}') comp_ms=$((c1-c0)) comp_wake/s=$(awk -v a=$k0 -v b=$k1 -v t=$T 'BEGIN{printf "%.2f",(b-a)/t}')"
  kill $LP 2>/dev/null; wait $LP 2>/dev/null; kill $CP; sleep 1
done; done
