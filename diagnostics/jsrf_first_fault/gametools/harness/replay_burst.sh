#!/bin/zsh
# usage: replay_burst.sh <name> <padrec> <from_frame> <count> [--env K=V ...] -- capture as fast as possible from from_frame
S=${JSRF_RUNS:-$HOME/jsrf-build/runs}
H=/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration/diagnostics/jsrf_first_fault/stage_harness
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
name=$1; pad=$2; from=$3; count=$4; shift 4
/usr/bin/python3 $H/run.py --scenario $H/watch.json --observe --debug-seconds 1500 \
  --binary ${BIN:-$HOME/jsrf-build/JSRF.app/Contents/MacOS/jsrf-engine} \
  --game "$ICL/Jet Set Radio Future (US)" --hdd "$HOME/Library/Application Support/JSRF/hdd" \
  --out $S/$name --env SDL_AUDIODRIVER=no_such_driver --env "RECOMP_PAD_SCRIPT=@$pad" "$@" > $S/$name.console 2>&1 &
PY=$!
: > $S/$name.timeline
while kill -0 $PY 2>/dev/null; do
  f=$(/usr/bin/python3 -c "import json;print(json.load(open('$S/$name/status.json'))['frame'])" 2>/dev/null); f=${f:-0}
  [ $f -ge $from ] && grep -q "DEBUG READY" $S/$name.console 2>/dev/null && break
  sleep 0.5
done
for i in $(seq 1 $count); do
  out=$(/usr/bin/python3 $H/debug.py $S/$name capture 2>/dev/null)
  f=$(/usr/bin/python3 -c "import json;print(json.load(open('$S/$name/status.json'))['frame'])" 2>/dev/null)
  echo "$f $(echo "$out" | grep -o '"picture": "[^"]*"' | cut -d'"' -f4)" >> $S/$name.timeline
done
/usr/bin/python3 $H/debug.py $S/$name stop >/dev/null 2>&1
sleep 5; pkill -x jsrf-engine; wait $PY
echo "$name done" >> $S/arms.done
