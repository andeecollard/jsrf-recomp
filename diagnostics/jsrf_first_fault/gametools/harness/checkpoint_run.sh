#!/bin/zsh
# usage: checkpoint_run.sh <name> <hdd> <captures> <every_frames> [--env K=V ...]
S=${JSRF_RUNS:-$HOME/jsrf-build/runs}
H=/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration/diagnostics/jsrf_first_fault/stage_harness
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
name=$1; hdd=$2; n=$3; every=$4; shift 4
/usr/bin/python3 $H/run.py --scenario $(dirname $0)/load.json --debug-seconds 600 \
  --binary ${BIN:-/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration/build-macos/jsrf-first-fault/build-feav/jsrf_first_fault} \
  --game "$ICL/Jet Set Radio Future (US)" --hdd "$hdd" --out $S/$name --env SDL_AUDIODRIVER=no_such_driver "$@" > $S/$name.console 2>&1 &
PY=$!
until grep -q "DEBUG READY" $S/$name.console 2>/dev/null || ! kill -0 $PY 2>/dev/null; do sleep 0.5; done
: > $S/$name.timeline; next=0
for i in $(seq 1 $n); do
  while :; do f=$(/usr/bin/python3 -c "import json;print(json.load(open('$S/$name/status.json'))['frame'])" 2>/dev/null); f=${f:-0}; [ $f -ge $next ] && break; kill -0 $PY 2>/dev/null || break 2; sleep 0.2; done
  p=$(/usr/bin/python3 $H/debug.py $S/$name capture 2>/dev/null | grep -o '"picture": "[^"]*"' | cut -d'"' -f4)
  echo "$f $p" >> $S/$name.timeline; next=$((f+every))
done
/usr/bin/python3 $H/debug.py $S/$name stop >/dev/null 2>&1
sleep 5; pkill -f "$S/$name" ; pkill -x jsrf_first_fault; wait $PY
echo "$name done" >> $S/arms.done
