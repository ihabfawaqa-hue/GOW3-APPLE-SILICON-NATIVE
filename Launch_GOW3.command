#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
# Metal's performance HUD (FPS, frame time, GPU) stays on; MTL_HUD_ENABLED=0 hides it.
export MTL_HUD_ENABLED="${MTL_HUD_ENABLED:-1}"
cd "$DIR"

# Keep the known asset path and watchdog defaults while allowing bounded test
# runs to select their own capture directory and cadence.
# Machine-specific settings (the game directory) live in local.env, which Git ignores:
# copy local.env.example to local.env and edit it.
if [ -f "$DIR/local.env" ]; then . "$DIR/local.env"; fi
export GOW3_DIR="${GOW3_DIR:-$HOME/GOW3/gow3_cusa01623_extracted}"
export PS4_APP_DIR="${PS4_APP_DIR:-$GOW3_DIR}"
export PS4_STALL_WATCHDOG="${PS4_STALL_WATCHDOG:-3000}"
export PS4_SCREENSHOT_DIR="${PS4_SCREENSHOT_DIR:-./screenshots}"
export PS4_SCREENSHOT_EVERY="${PS4_SCREENSHOT_EVERY:-60}"

echo "=========================================================="
echo "  God of War III Remastered - Native Apple Silicon ARM64  "
echo "  Target Architecture: Apple Silicon (Metal AOT)         "
echo "  Asset Root: $GOW3_DIR"
echo "=========================================================="

if [ ! -d "$GOW3_DIR" ]; then
    echo "[!] Error: Game assets not found at $GOW3_DIR"
    echo "[!] Please ensure the external drive / volume is mounted."
    read -p "Press Enter to exit..."
    exit 1
fi

chmod +x "./ps4_app"
exec "./ps4_app" "$@"
