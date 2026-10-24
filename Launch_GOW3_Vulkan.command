#!/bin/bash
# Opt-in launcher for the candidate GoW3/shadPS4 Vulkan backend (KosmicKrisp on Apple
# Silicon). Launch_GOW3.command keeps the default Metal path. Every session's output is
# also written to a log under GOW3Lab/launcher-logs so a crash leaves evidence behind.
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

export GOW3_DIR="${GOW3_DIR:-/Volumes/1TB/gow3_cusa01623_extracted}"
export PS4_APP_DIR="${PS4_APP_DIR:-$GOW3_DIR}"
export PS4_STALL_WATCHDOG="${PS4_STALL_WATCHDOG:-3000}"
export PS4_SCREENSHOT_DIR="${PS4_SCREENSHOT_DIR:-./screenshots}"
export PS4_SCREENSHOT_EVERY="${PS4_SCREENSHOT_EVERY:-60}"

LAB_DIR="$HOME/Library/Application Support/GOW3Lab"
export PS4_GOW3GPU_BACKEND=gow3gpu-vulkan
export PS4_GOW3GPU_LIBRARY="${PS4_GOW3GPU_LIBRARY:-/tmp/gow3gpu-arm64-dense-range-build/libgow3gpu.dylib}"
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/tmp/mesa-kosmickrisp-install312/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json}"
export PS4_GOW3GPU_USER_DIR="${PS4_GOW3GPU_USER_DIR:-$LAB_DIR/candidate-user}"
export GOW3_GPU_USER_DIR="${GOW3_GPU_USER_DIR:-$PS4_GOW3GPU_USER_DIR}"

echo "=========================================================="
echo "  God of War III Remastered - Native Apple Silicon ARM64  "
echo "  Graphics: GoW3/shadPS4 Vulkan (KosmicKrisp)            "
echo "  Asset Root: $GOW3_DIR"
echo "=========================================================="

missing=0
for path in "$GOW3_DIR" "$PS4_GOW3GPU_LIBRARY" "$VK_DRIVER_FILES"; do
    if [ ! -e "$path" ]; then
        echo "[!] Missing: $path"
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    echo "[!] Mount the game volume and rebuild the GPU library / KosmicKrisp driver if /tmp was cleared."
    read -p "Press Enter to exit..."
    exit 1
fi

LOG_DIR="$LAB_DIR/launcher-logs"
mkdir -p "$LOG_DIR" "$PS4_GOW3GPU_USER_DIR"
LOG="$LOG_DIR/session_$(date +%Y%m%d_%H%M%S).log"
echo "  Log: $LOG"

chmod +x "./ps4_app"
"./ps4_app" "$@" > >(tee -a "$LOG") 2>&1
status=$?
echo "[launcher] ps4_app exited with status $status" | tee -a "$LOG"
if [ "$status" -ne 0 ]; then
    read -p "The game exited with an error. Press Enter to close..."
fi
exit "$status"
