#!/bin/bash
# ab_late.sh <tag> [env...]: 250 s run; FPS, host-copy blocking and recorder blocking of windows from 175 s on.
S="$(cd "$(dirname "$0")" && pwd)"
tag=$1; shift
cd $S && python3 bench_cfg.py "$tag" 250 300 "$@" 2>&1 | head -1
R=$(ls -t ~/Library/Application\ Support/GOW3Lab/runs | head -1); f=~/Library/Application\ Support/GOW3Lab/runs/$R/stdout.log
grep "Frame stats" "$f" | awk 'NR>=35' | sed -nE 's/Frame stats: ([0-9.]+) FPS.* ([0-9]+) draws\/frame.*blocked: recorder ([0-9.]+)%, host copies ([0-9.]+)%.*/\1 \2 \3 \4/p' |
  awk '$2>1500 {n++; f+=$1; r+=$3; h+=$4} END {if(n) printf "  late windows %d: %.1f FPS, recorder %.1f%%, host copies %.1f%%\n", n, f/n, r/n, h/n}'
echo "  windows $(grep -c 'Frame stats' "$f")/50, FATAL $(grep -c FATAL ~/Library/Application\ Support/GOW3Lab/runs/$R/stderr.log)"
