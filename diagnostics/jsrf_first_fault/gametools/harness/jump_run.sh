#!/bin/zsh
# usage: jump_run.sh <name> <captures> <gap-seconds> <max-wait> [--env K=V ...]
# Garage boot; once "[CHAPTER-JUMP] fired" (or the scenario ends, unarmed),
# take <captures> captures <gap> s apart, then stop.
S=${JSRF_RUNS:-$HOME/jsrf-build/runs}
R=/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration
H=$R/diagnostics/jsrf_first_fault/stage_harness
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
name=$1 n=$2 gap=$3 maxw=$4; shift 4
O=$S/$name
/usr/bin/python3 $H/run.py --scenario $H/garage.json --debug-seconds 400 \
  --binary $R/build-macos/jsrf-first-fault/build-feav/jsrf_first_fault \
  --game "$ICL/Jet Set Radio Future (US)" --hdd "$HOME/Library/Application Support/JSRF/hdd" \
  --out $O --env SDL_AUDIODRIVER=no_such_driver "$@" > $O.console 2>&1 &
PY=$!
T0=$SECONDS
while (( SECONDS - T0 < maxw )); do
  grep -q "CHAPTER-JUMP\] fired" $O/runtime.log 2>/dev/null && break
  kill -0 $PY 2>/dev/null || break
  sleep 2
done
echo "trigger t=$((SECONDS-T0)) $(date +%T)" >> $O.console
for k in $(seq 1 $n); do sleep $gap; /usr/bin/python3 $H/debug.py $O capture > $O.capture$k 2>&1; done
/usr/bin/python3 $H/debug.py $O stop >/dev/null 2>&1
sleep 8; kill $PY 2>/dev/null; sleep 2; pkill -f "build-feav/jsrf_first_fault"
wait $PY
echo "$name done $(date +%T)" >> $S/runs.done
