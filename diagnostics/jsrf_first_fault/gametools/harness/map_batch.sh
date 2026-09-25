#!/bin/zsh
# usage: map_batch.sh <targets-file> [freeplay-s=50] [budget-s=360]
# Runs map_run.sh over "C:MM stage label" lines, skipping targets already done; a
# crash or hang is rerun once (name.rerun) to tell intermittent from reproducible.
S=${JSRF_RUNS:-$HOME/jsrf-build/runs/map}
D=${0:A:h}
fp=${2:-50} budget=${3:-360}
while read -r jump stage label; do
  [[ -z $jump || $jump = \#* ]] && continue
  c=${jump%%:*} m=${jump##*:}; n=$(printf 'm%02d%02d' $c $m)
  grep -q "^$n $jump attempt .* done" $S/runs.done 2>/dev/null && continue
  $D/map_run.sh $n $jump $fp $budget < /dev/null
  r=$(grep -h '^reason=' $S/runs/$n.status $S/runs/$n.retry.status 2>/dev/null | tail -1 | sed 's/reason=\([a-z]*\).*/\1/')
  if [[ $r = crash || $r = hang ]]; then $D/map_run.sh $n.rerun $jump $fp $budget < /dev/null; fi
done < $1
echo "batch $1 done $(date +%T)" >> $S/runs.done
