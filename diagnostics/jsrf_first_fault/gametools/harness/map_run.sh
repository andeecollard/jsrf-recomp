#!/bin/zsh
# usage: map_run.sh <name> <C:M> <freeplay-s> <budget-s> [--env K=V ...]   (e.g. 50 360)
# One game-map target (25 Sep 2026): Garage boot, RECOMP_CHAPTER_JUMP=<C:M>, glitch
# watch armed; after "[CHAPTER-JUMP] fired" wait for the mission to sit in state 0x0F
# (free play), then keep it there <freeplay-s> with captures at +12 s, +32 s and the end.
# Ends early on a crash (guest process gone) or a hang ([FRAME] flips not advancing for
# 20 s; the periodic report keeps printing through a hang, so flips are the signal). Writes <out>.status (key=value lines) and deletes the staged HDD copy.
# Retries once when the jump never fires (the title input flake).
# MAP_PRESS_A=1: when the mission has sat outside 0x0F for 40 s (a briefing card waiting
# for the A button), press A, at most every 20 s.
S=${JSRF_RUNS:-$HOME/jsrf-build/runs/map}
R=/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration
H=$R/diagnostics/jsrf_first_fault/stage_harness
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
setopt nullglob
name=$1 jump=$2 fp=$3 budget=$4; shift 4
mkdir -p $S/runs
flips() { grep -a '^  \[FRAME\] flips=' $1/runtime.log 2>/dev/null | tail -1 | sed -n 's/.*flips=\([0-9]*\).*/\1/p'; }
cap() { /usr/bin/python3 $H/debug.py $O capture > $O.cap-$1 2>&1; }
for attempt in 1 2; do
  O=$S/runs/$name; [[ $attempt = 2 ]] && O=$S/runs/$name.retry
  rm -rf $O $O.*; 
  /usr/bin/python3 $H/run.py --scenario $H/garage.json --debug-seconds $((budget + 500)) \
    --binary $R/build-macos/jsrf-first-fault/build-feav/jsrf_first_fault \
    --game "$ICL/Jet Set Radio Future (US)" --hdd "$HOME/Library/Application Support/JSRF/hdd" \
    --out $O --env SDL_AUDIODRIVER=no_such_driver --env RECOMP_GLITCH_WATCH=1 \
    --env RECOMP_FLIGHT_DIR=$O/glitch --env RECOMP_CHAPTER_JUMP=$jump "$@" > $O.console 2>&1 &
  PY=$!
  T0=$SECONDS fired=0
  while (( SECONDS - T0 < 300 )); do
    grep -aq "CHAPTER-JUMP\] fired" $O/runtime.log 2>/dev/null && { fired=1; break; }
    kill -0 $PY 2>/dev/null || break
    sleep 2
  done
  echo "jump=$jump attempt=$attempt fired=$fired t_fired=$((SECONDS-T0)) at=$(date +%T)" >> $O.status
  reason=none
  if (( fired )); then
    T1=$SECONDS t0f=0 lastline=0 st="" k=0 pre=0 lf=-1 lfT=$SECONDS first0f=-1
    while true; do
      if ! pgrep -f "build-feav/jsrf_first_fault" >/dev/null; then reason=crash; break; fi
      f=$(flips $O); if [[ $f != $lf ]]; then lf=$f lfT=$SECONDS; fi
      if (( SECONDS - lfT >= 20 )); then reason=hang; break; fi
      c=$(grep -ac "CHAPTER-JUMP\] frame" $O/runtime.log)
      st=$(grep -a "CHAPTER-JUMP\] frame" $O/runtime.log | tail -1 | sed -n 's/.*-> \(0x[0-9A-F]*\).*/\1/p')
      if [[ $c != $lastline ]]; then lastline=$c; t0f=$SECONDS; k=0; fi
      if [[ $st = 0x0F ]]; then
        (( first0f < 0 )) && first0f=$((SECONDS - T1))
        d=$((SECONDS - t0f))
        if (( d >= 12 && k == 0 )); then cap fp1; k=1; fi
        if (( d >= 32 && k == 1 )); then cap fp2; k=2; fi
        if (( d >= fp )); then reason=freeplay; break; fi
      else
        if (( SECONDS - T1 >= 90 && pre == 0 )); then cap pre90; pre=1; fi
        if [[ -n $MAP_PRESS_A ]] && (( SECONDS - t0f >= 40 && SECONDS - ${lastA:-0} >= 20 )); then
          /usr/bin/python3 $H/debug.py $O press A >> $O.presses 2>&1; lastA=$SECONDS
          echo "pressed A at t=$((SECONDS-T1)) state $st" >> $O.status
        fi
      fi
      if (( SECONDS - T1 >= budget )); then reason=budget; break; fi
      sleep 2
    done
    [[ $reason != crash ]] && cap end
    echo "reason=$reason t_after_fired=$((SECONDS-T1)) first_0F=$first0f last_state=$st lines=$lastline flips=$lf" >> $O.status
  fi
  /usr/bin/python3 $H/debug.py $O stop >/dev/null 2>&1
  sleep 6; kill $PY 2>/dev/null; sleep 2; pkill -f "build-feav/jsrf_first_fault"; sleep 1
  wait $PY 2>/dev/null
  rm -rf $O/hdd
  echo "$name $jump attempt $attempt fired=$fired reason=$reason done $(date +%T)" >> $S/runs.done
  (( fired )) && break
done
