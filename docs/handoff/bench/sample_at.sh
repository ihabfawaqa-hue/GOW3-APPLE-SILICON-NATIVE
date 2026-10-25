#!/bin/bash
# sample_at.sh <delay-s> <seconds> <out>: sample the running ps4_app after a delay.
sleep "$1"
pid=$(pgrep -n ps4_app) || { echo "no ps4_app"; exit 1; }
sample "$pid" "$2" 1 -mayDie -file "$3" >/dev/null 2>&1
echo "sampled $pid -> $3"
