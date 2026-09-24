#!/bin/zsh
# usage: replay_timeline.sh <name> <padrec> <last_frame> <every> [--env K=V ...]
S=${JSRF_RUNS:-$HOME/jsrf-build/runs}
H=/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration/diagnostics/jsrf_first_fault/stage_harness
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
name=$1; pad=$2; last=$3; every=$4; shift 4
/usr/bin/python3 $H/run.py --scenario $H/watch.json --observe --debug-seconds 1500 \
  --binary ${BIN:-$HOME/jsrf-build/JSRF.app/Contents/MacOS/jsrf-engine} \
  --game "$ICL/Jet Set Radio Future (US)" --hdd "$HOME/Library/Application Support/JSRF/hdd" \
  --out $S/$name --env SDL_AUDIODRIVER=no_such_driver --env "RECOMP_PAD_SCRIPT=@$pad" "$@" > $S/$name.console 2>&1 &
PY=$!
next=$every
: > $S/$name.timeline
for i in $(seq 1 2000); do
  f=$(/usr/bin/python3 -c "import json;print(json.load(open('$S/$name/status.json'))['frame'])" 2>/dev/null)
  f=${f:-0}
  if [ $f -ge $next ] && grep -q "DEBUG READY" $S/$name.console 2>/dev/null; then
    p=$(/usr/bin/python3 $H/debug.py $S/$name capture 2>/dev/null | grep -o '"picture": "[^"]*"' | cut -d'"' -f4)
    echo "$f $p" >> $S/$name.timeline
    next=$(( (f/every+1)*every ))
  fi
  [ $f -gt $last ] && break
  kill -0 $PY 2>/dev/null || break
  sleep 1
done
/usr/bin/python3 $H/debug.py $S/$name stop >/dev/null 2>&1
sleep 5; pkill -x jsrf-engine; wait $PY
echo "$name frame=$f" >> $S/arms.done
