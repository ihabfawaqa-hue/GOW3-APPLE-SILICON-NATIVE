#!/bin/bash
# Installs the built GoW3 GPU library and the KosmicKrisp Vulkan driver into a versioned,
# reboot-safe runtime directory and points `current` at it. Launch_GOW3_Vulkan.command loads
# from `current`; older versions stay in place for rollback (repoint the symlink).
#
#   scripts/lab/install_runtime.sh            # install from the default build locations
#   GOW3GPU_BUILD_DIR=... KOSMICKRISP_INSTALL_DIR=... scripts/lab/install_runtime.sh
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
GPU_BUILD="${GOW3GPU_BUILD_DIR:-/tmp/gow3gpu-arm64-dense-range-build}"
KK_INSTALL="${KOSMICKRISP_INSTALL_DIR:-/tmp/mesa-kosmickrisp-install312}"
KK_SOURCE="${KOSMICKRISP_SOURCE_DIR:-/tmp/mesa-kosmickrisp-src}"
RUNTIME_ROOT="$HOME/Library/Application Support/GOW3Lab/runtime"

GPU_LIB="$GPU_BUILD/libgow3gpu.dylib"
KK_LIB="$KK_INSTALL/lib/libvulkan_kosmickrisp.dylib"
for path in "$GPU_LIB" "$KK_LIB"; do
    if [ ! -f "$path" ]; then
        echo "install_runtime: missing $path (build it first)" >&2
        exit 1
    fi
done
for path in "$GPU_LIB" "$KK_LIB"; do
    if [ "$(lipo -archs "$path")" != "arm64" ]; then
        echo "install_runtime: $path is not arm64" >&2
        exit 1
    fi
done

# The driver must be built with the raised command-buffer BO pool (scripts/lab/patches/
# kosmickrisp-cmd-bo-pool.patch); at the stock 32 it frees and allocates BOs mid-frame.
# KOSMICKRISP_SKIP_PATCH_CHECK=1 installs a driver whose source tree is not available.
POOL_HDR="$KK_SOURCE/src/kosmickrisp/vulkan/kk_cmd_pool.h"
if [ "${KOSMICKRISP_SKIP_PATCH_CHECK:-0}" != "1" ]; then
    POOL_MAX="$(sed -n 's/^#define KK_CMD_POOL_BO_MAX[[:space:]]*\([0-9]*\).*/\1/p' "$POOL_HDR" 2>/dev/null)"
    if [ "$POOL_MAX" != "1024" ]; then
        echo "install_runtime: KK_CMD_POOL_BO_MAX is '${POOL_MAX:-unreadable}' in $POOL_HDR, expected 1024;" >&2
        echo "  apply scripts/lab/patches/kosmickrisp-cmd-bo-pool.patch and rebuild, or set KOSMICKRISP_SKIP_PATCH_CHECK=1" >&2
        exit 1
    fi
fi

REV="$(git -C "$REPO" rev-parse --short HEAD)"
DIRTY=""
if ! git -C "$REPO" diff --quiet -- third_party/gow3gpu runtime; then
    DIRTY="-dirty"
fi
ID="$(date +%Y%m%d-%H%M%S)-$REV$DIRTY"
DEST="$RUNTIME_ROOT/$ID"
mkdir -p "$DEST"

cp "$GPU_LIB" "$DEST/libgow3gpu.dylib"
cp "$KK_LIB" "$DEST/libvulkan_kosmickrisp.dylib"
API_VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ICD"]["api_version"])' \
    "$KK_INSTALL/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json")"
# The Vulkan loader resolves a relative library_path against the JSON file's directory.
cat > "$DEST/kosmickrisp_icd.json" <<EOF
{
    "ICD": {
        "api_version": "$API_VERSION",
        "library_path": "./libvulkan_kosmickrisp.dylib"
    },
    "file_format_version": "1.0.1"
}
EOF

MESA_REV="$(git -C "$KK_SOURCE" rev-parse HEAD 2>/dev/null || echo unknown)"
{
    echo "runtime_id=$ID"
    echo "installed=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "source_revision=$(git -C "$REPO" rev-parse HEAD)$DIRTY"
    echo "gpu_build_dir=$GPU_BUILD"
    echo "kosmickrisp_mesa_revision=$MESA_REV"
    echo "kosmickrisp_api_version=$API_VERSION"
    echo "macos=$(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    (cd "$DEST" && shasum -a 256 libgow3gpu.dylib libvulkan_kosmickrisp.dylib kosmickrisp_icd.json)
} > "$DEST/MANIFEST.txt"

ln -sfn "$ID" "$RUNTIME_ROOT/current"
echo "installed $DEST"
echo "current -> $ID"
