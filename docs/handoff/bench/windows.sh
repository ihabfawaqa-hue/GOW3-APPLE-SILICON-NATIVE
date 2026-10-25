#!/bin/bash
# windows.sh <tag> [env...]: 250 s run; prints gameplay frame-stat windows (fps/worst) and host-copy waits.
S="$(cd "$(dirname "$0")" && pwd)"
tag=$1; shift
cd $S && python3 bench_cfg.py "$tag" 250 300 "$@" 2>&1 | head -1
R=$(ls -t ~/Library/Application\ Support/GOW3Lab/runs | head -1); D=~/Library/Application\ Support/GOW3Lab/runs/$R
grep "Frame stats" "$D/stdout.log" | sed -E 's/.*Frame stats: ([0-9.]+) FPS, worst frame ([0-9.]+) ms.*GPU thread [0-9.]+ us\/draw, ([0-9]+) draws\/frame.*GPU ticks ([0-9.]+)%.*/\1 \2 \3 \4/' | awk '$3>1500 {n++; s+=$1; printf "%s/%s ", $1, $2} END {if(n) printf "\n  mean of %d windows: %.1f FPS\n", n, s/n}'
echo "  host-copy waits in stall frames: $(grep '^Stall:' "$D/stdout.log" | sed -E 's/.*host copies ([0-9.]+) ms.*/\1/' | awk '{s+=$1;n++} END {if(n) printf "%.1f ms avg over %d", s/n, n}')"
echo "  FATAL: $(grep -c FATAL "$D/stderr.log")"
