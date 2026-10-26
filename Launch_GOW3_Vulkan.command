#!/bin/bash
# Opt-in launcher for the candidate GoW3/shadPS4 Vulkan backend (KosmicKrisp on Apple
# Silicon). Launch_GOW3.command keeps the default Metal path. Every session's output is
# also written to a log under GOW3Lab/launcher-logs so a crash leaves evidence behind.
DIR="$(cd "$(dirname "$0")" && pwd)"
# Metal's performance HUD (FPS, frame time, GPU) stays on; MTL_HUD_ENABLED=0 hides it.
export MTL_HUD_ENABLED="${MTL_HUD_ENABLED:-1}"
cd "$DIR"

export GOW3_DIR="${GOW3_DIR:-/Volumes/1TB/gow3_cusa01623_extracted}"
export PS4_APP_DIR="${PS4_APP_DIR:-$GOW3_DIR}"
export PS4_STALL_WATCHDOG="${PS4_STALL_WATCHDOG:-3000}"
export PS4_SCREENSHOT_DIR="${PS4_SCREENSHOT_DIR:-./screenshots}"
export PS4_SCREENSHOT_EVERY="${PS4_SCREENSHOT_EVERY:-60}"
# The macOS eboot runs at its native 60 Hz; keep presenter caps aligned unless
# the caller explicitly identifies a different patched engine rate.
export GOW3_ENGINE_FPS="${GOW3_ENGINE_FPS:-60}"

# Borderless fullscreen by default. Set GOW3_HIGH_DPI=1 to opt into a Retina
# drawable; GOW3_OUTPUT_RES=2560x1440 selects a 1440p target, and GOW3_FSR1=1
# enables FSR1 scaling. Keep FSR1 off when combining 1440p with high-DPI: that
# combination has produced black output and a presenter crash in local runs.
# Output scaling preserves the game's 16:9 aspect ratio.
# The default remains the lower-cost 1x/1080p path until high-DPI performance is measured.
# GOW3_DISPLAY=windowed opens a window instead (as does display_mode in gow3.ini when this is off).
if [ "${GOW3_DISPLAY:-}" != "windowed" ]; then
    export GOW3_FULLSCREEN=1
fi

LAB_DIR="$HOME/Library/Application Support/GOW3Lab"
# Versioned GPU library and KosmicKrisp driver (scripts/lab/install_runtime.sh); repoint
# runtime/current at an older version, or set these variables, to roll back.
RUNTIME_DIR="$LAB_DIR/runtime/current"
export PS4_GOW3GPU_BACKEND=gow3gpu-vulkan
export PS4_GOW3GPU_LIBRARY="${PS4_GOW3GPU_LIBRARY:-$RUNTIME_DIR/libgow3gpu.dylib}"
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-$RUNTIME_DIR/kosmickrisp_icd.json}"
export PS4_GOW3GPU_USER_DIR="${PS4_GOW3GPU_USER_DIR:-$LAB_DIR/candidate-user}"
export GOW3_GPU_USER_DIR="${GOW3_GPU_USER_DIR:-$PS4_GOW3GPU_USER_DIR}"

echo "=========================================================="
echo "  God of War III Remastered - Native Apple Silicon ARM64  "
echo "  Graphics: GoW3/shadPS4 Vulkan (KosmicKrisp)            "
echo "  Asset Root: $GOW3_DIR"
echo "  GPU runtime: $(readlink "$RUNTIME_DIR" 2>/dev/null || echo "$PS4_GOW3GPU_LIBRARY")"
echo "=========================================================="

missing=0
for path in "$GOW3_DIR" "$PS4_GOW3GPU_LIBRARY" "$VK_DRIVER_FILES"; do
    if [ ! -e "$path" ]; then
        echo "[!] Missing: $path"
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    echo "[!] Mount the game volume, or install the GPU runtime: scripts/lab/install_runtime.sh"
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
