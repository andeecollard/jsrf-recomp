#!/bin/zsh
# usage: replay_arm.sh <name> [--env K=V ...]
S=${JSRF_RUNS:-$HOME/jsrf-build/runs}
H=/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration/diagnostics/jsrf_first_fault/stage_harness
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
name=$1; shift
/usr/bin/python3 $H/run.py --scenario $H/watch.json --observe --debug-seconds 300 \
  --binary ${BIN:-$HOME/jsrf-build/JSRF.app/Contents/MacOS/jsrf-engine} \
  --game "$ICL/Jet Set Radio Future (US)" --hdd "$HOME/Library/Application Support/JSRF/hdd" \
  --out $S/$name --env SDL_AUDIODRIVER=no_such_driver \
  --env "RECOMP_PAD_SCRIPT=@$HOME/Library/Application Support/JSRF/padrec/graffiti-2026-09-24_1918.padrec" "$@" > $S/$name.console 2>&1 &
PY=$!
for i in $(seq 1 100); do f=$(/usr/bin/python3 -c "import json;print(json.load(open('$S/$name/status.json'))['frame'])" 2>/dev/null); [ "${f:-0}" -gt 5750 ] && break; sleep 3; done
/usr/bin/python3 $H/debug.py $S/$name capture > $S/$name.capture1 2>&1
sleep 4
/usr/bin/python3 $H/debug.py $S/$name capture > $S/$name.capture2 2>&1
/usr/bin/python3 $H/debug.py $S/$name stop >/dev/null 2>&1
sleep 5; pkill -x jsrf-engine; wait $PY
echo "$name frame=$f" >> $S/arms.done
