#!/bin/bash
# ab_argon.sh [rounds=2] [cooldown_s=120]: Operation Spartan (Argon) configuration vs control, alternating
# so heat and background load average out. Run on an idle machine (load average < 2, nothing else open).
#
#   base      guest TSO checks ON  (binary built with GOW3_GUEST_TSO_CHECKS=1), default env
#   tso_off   guest TSO checks OFF (current default build)
#   +qos      tso_off + PS4_THREAD_QOS=brbDriver=default, GOW3_COPY_QOS=initiated
#   +prep2    +qos + GOW3_PREP_WORKERS=2
#
# The TSO setting is compiled in, so the script swaps ./ps4_app between two binaries:
#   ps4_app.tso_on  must exist (GOW3_GUEST_TSO_CHECKS=1 python3 tools/generate_build_ninja.py && ninja ps4_app
#                   && cp ps4_app ps4_app.tso_on, then rebuild the default).
set -u
S="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$S/../../.." && pwd)"
rounds=${1:-2}; cool=${2:-120}
cd "$REPO" || exit 1
[ -f ps4_app.tso_on ] || { echo "missing ps4_app.tso_on (see header)"; exit 1; }
cp ps4_app ps4_app.tso_off
trap 'cp ps4_app.tso_off ps4_app' EXIT
LIB=PS4_GOW3GPU_LIBRARY=${GOW3_LIB:-$HOME/Library/Application Support/GOW3Lab/runtime/current/libgow3gpu.dylib}
run() {  # tag binary env...
  tag=$1; bin=$2; shift 2
  cp "ps4_app.$bin" ps4_app
  echo "load before $tag: $(uptime | sed 's/.*averages: //')"
  (cd "$S" && python3 bench_cfg.py "$tag" 250 300 "$LIB" "$@" 2>&1 | sed -n 2p)
  sleep "$cool"
}
for r in $(seq 1 "$rounds"); do
  run "base_$r"    tso_on  GOW3_X=0
  run "tsooff_$r"  tso_off GOW3_X=0
  run "qos_$r"     tso_off 'PS4_THREAD_QOS=brbDriver=default' GOW3_COPY_QOS=initiated
  run "prep2_$r"   tso_off 'PS4_THREAD_QOS=brbDriver=default' GOW3_COPY_QOS=initiated GOW3_PREP_WORKERS=2
done
