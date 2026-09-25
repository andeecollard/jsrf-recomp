#!/bin/zsh
# chapter intros first, then the other class-A jumps from the catalogue
cd ${JSRF_RUNS:-$HOME/jsrf-build/runs}
for t in "$@"; do
  n=s${t/:/m}
  [[ -f runs/$n/runtime.log ]] && grep -q "fired" runs/$n/runtime.log && continue
  ./gw_run.sh $n 600 30 60 --env RECOMP_CHAPTER_JUMP=$t
done
echo "sweep $* done" >> runs.done
