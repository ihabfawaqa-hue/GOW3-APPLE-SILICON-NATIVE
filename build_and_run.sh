#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ "$(uname -m)" != "arm64" ]]; then
    echo "error: this build requires native Apple Silicon (arm64); Rosetta is not supported" >&2
    exit 1
fi
for tool in clang clang++ ninja python3 file lipo; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required tool '$tool' is unavailable; install the Xcode command-line tools and Ninja, then retry" >&2
        exit 1
    fi
done
if [[ ! -f "$SCRIPT_DIR/guest_image.bin" ]]; then
    echo "error: local guest_image.bin input is missing from $SCRIPT_DIR" >&2
    exit 1
fi

python3 "$SCRIPT_DIR/tools/generate_build_ninja.py"
ninja -j4 ps4_app

if ! lipo -info "$SCRIPT_DIR/ps4_app" | grep -q 'arm64'; then
    echo "error: build output is not an ARM64 executable" >&2
    exit 1
fi
file "$SCRIPT_DIR/ps4_app"
exec /bin/bash "$SCRIPT_DIR/Launch_GOW3.command" "$@"
