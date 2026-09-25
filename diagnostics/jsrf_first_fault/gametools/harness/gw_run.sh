#!/bin/zsh
# usage: gw_run.sh <name> <budget-s after fired> <quiet-s> <capture-gap-s> [--env K=V ...]
# Garage boot with RECOMP_GLITCH_WATCH=1; after "[CHAPTER-JUMP] fired", keep the game
# running until the mission has sat in state 0x0F (RunCmds) for <quiet-s> with no
# state change, or <budget-s> has passed; a capture every <capture-gap-s>. Retries
# once when the jump never fires (the title input flake).
S=${JSRF_RUNS:-$HOME/jsrf-build/runs}
R=/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration
H=$R/diagnostics/jsrf_first_fault/stage_harness
# JSRF_BIN points a run at a private build (e.g. a worktree's) instead of the live one.
BIN=${JSRF_BIN:-$R/build-macos/jsrf-first-fault/build-feav/jsrf_first_fault}
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
name=$1 budget=$2 quiet=$3 gap=$4; shift 4
for attempt in 1 2; do
  O=$S/runs/$name; [[ $attempt = 2 ]] && O=$S/runs/$name.retry
  rm -rf $O; mkdir -p $S/runs
  /usr/bin/python3 $H/run.py --scenario $H/garage.json --debug-seconds $((budget + 400)) \
    --binary $BIN \
    --game "$ICL/Jet Set Radio Future (US)" --hdd "$HOME/Library/Application Support/JSRF/hdd" \
    --out $O --env SDL_AUDIODRIVER=no_such_driver --env RECOMP_GLITCH_WATCH=1 \
    --env RECOMP_FLIGHT_DIR=$O/glitch "$@" > $O.console 2>&1 &
  PY=$!
  T0=$SECONDS fired=0
  while (( SECONDS - T0 < 300 )); do
    grep -aq "CHAPTER-JUMP\] fired" $O/runtime.log 2>/dev/null && { fired=1; break; }
    kill -0 $PY 2>/dev/null || break
    sleep 2
  done
  echo "fired=$fired t=$((SECONDS-T0)) $(date +%T)" >> $O.status
  if (( fired )); then
    T1=$SECONDS last=0 lastchange=$SECONDS k=0 nextcap=$((SECONDS + gap))
    while (( SECONDS - T1 < budget )); do
      kill -0 $PY 2>/dev/null || break
      c=$(grep -ac "CHAPTER-JUMP\] frame" $O/runtime.log)
      if [[ $c != $last ]]; then last=$c lastchange=$SECONDS; fi
      st=$(grep -a "CHAPTER-JUMP\] frame" $O/runtime.log | tail -1 | sed -n 's/.*-> \(0x[0-9A-F]*\).*/\1/p')
      if [[ $st = 0x0F ]] && (( SECONDS - lastchange >= quiet && SECONDS - T1 > 30 )); then break; fi
      if (( SECONDS >= nextcap )); then k=$((k+1)); /usr/bin/python3 $H/debug.py $O capture > $O.capture$k 2>&1; nextcap=$((SECONDS + gap)); fi
      sleep 2
    done
    echo "ended t=$((SECONDS-T1)) after fired, last state $st $(date +%T)" >> $O.status
    /usr/bin/python3 $H/debug.py $O capture > $O.capturelast 2>&1
  fi
  /usr/bin/python3 $H/debug.py $O stop >/dev/null 2>&1
  sleep 8; kill $PY 2>/dev/null; sleep 2; pkill -f "$BIN"
  wait $PY
  echo "$name attempt $attempt fired=$fired done $(date +%T)" >> $S/runs.done
  (( fired )) && break
done
