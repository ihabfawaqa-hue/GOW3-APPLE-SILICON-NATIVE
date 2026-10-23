#!/usr/bin/env bash
# Builds the recompiler probe, translates a captured GoW3 draw, and checks the MSL with Metal.
#
# usage: verify_probe.sh <gpu-captures-dir> <build-dir>
#   <gpu-captures-dir>  e.g. runs/<run>/gpu-captures (must hold draw_largest_*.bin)
#   <build-dir>         object files, the probe binary and outputs go here (created if missing)
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <gpu-captures-dir> <build-dir>" >&2
    exit 2
fi

HERE="$(cd "$(dirname "$0")/../.." && pwd)"   # gpu/shadps4-recompiler
CAPTURE="$(cd "$1" && pwd)"
BUILD="$2"
mkdir -p "$BUILD/obj" "$BUILD/out"

FLAGS=(-std=c++23 -O1 -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
       -Wno-sign-compare -Wno-missing-field-initializers -Wno-deprecated-declarations)
INC=(-I"$HERE/shims" -I"$HERE" -I"$HERE/src" -I"$HERE/sirit/include" -I"$HERE/sirit/src"
     -I"$HERE/SPIRV-Headers" -I/opt/homebrew/include -I/opt/homebrew/include/spirv_cross)

# Every upstream .cpp the recompiler needs, except the x86-64-only flatten pass (replaced by
# port/flatten_userdata_arm64.cpp), plus sirit, the one amdgpu format unit, and the port files.
cd "$HERE"
SOURCES=$(
    {
        find src/shader_recompiler -name '*.cpp' -not -name 'flatten_extended_userdata_pass.cpp'
        find sirit/src -name '*.cpp'
        echo src/video_core/amdgpu/pixel_format.cpp
        find port -name '*.cpp'
    } | sort
)

compile_unit() {
    local src="$1"
    local obj="$BUILD/obj/$(echo "$src" | tr / _ | sed 's/\.cpp$/.o/')"
    clang++ "${FLAGS[@]}" "${INC[@]}" -c "$HERE/$src" -o "$obj"
}

JOBS="$(sysctl -n hw.ncpu)"
echo "[verify] compiling $(echo "$SOURCES" | wc -l | tr -d ' ') units with $JOBS jobs"
pids=()
for src in $SOURCES; do
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
        sleep 0.2
    done
    compile_unit "$src" &
    pids+=("$!")
done
status=0
for pid in "${pids[@]}"; do
    wait "$pid" || status=1
done
if [ "$status" -ne 0 ]; then
    echo "[verify] compile failed" >&2
    exit 1
fi

echo "[verify] linking $BUILD/recomp_probe"
find "$BUILD/obj" -name '*.o' | sort | xargs clang++ -o "$BUILD/recomp_probe" \
    -L/opt/homebrew/lib -lfmt -lspirv-cross-msl -lspirv-cross-core -lspirv-cross-glsl

echo "[verify] translating captured draw from $CAPTURE"
"$BUILD/recomp_probe" "$CAPTURE" "$BUILD/out"

echo "[verify] compiling emitted MSL with the Metal compiler"
for stage in vs ps; do
    xcrun -sdk macosx metal -std=macos-metal2.3 -c "$BUILD/out/$stage.metal" \
        -o "$BUILD/out/$stage.air"
    echo "[verify] $stage.metal -> $stage.air OK"
done
