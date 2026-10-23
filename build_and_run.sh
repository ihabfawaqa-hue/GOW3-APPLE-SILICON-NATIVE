#!/usr/bin/env bash
set -e

# ==============================================================================
# PROJECT SPARTAN: God of War III Remastered - Apple Silicon Native ARM64
# Build and Launch Script for macOS (Apple Silicon M1/M2/M3/M4)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================================="
echo "  God of War III Remastered - Native Apple Silicon ARM64   "
echo "  Target Architecture: Apple Silicon (AArch64 / Metal)    "
echo "  Rosetta: DISABLED | Emulation: NONE (Pure Native AOT)   "
echo "=========================================================="

# 1. Check Architecture
ARCH=$(uname -m)
if [ "$ARCH" != "arm64" ]; then
    echo "[!] Error: This port is strictly optimized for Apple Silicon (arm64)."
    echo "    Current machine reports: $ARCH"
    exit 1
fi

# 2. Check Xcode Command Line Tools
if ! command -v clang &> /dev/null; then
    echo "[*] Xcode Command Line Tools not detected. Installing..."
    xcode-select --install
    echo "[!] Please re-run this script after the Xcode tools installation completes."
    exit 1
fi

# 3. Check and install Homebrew dependencies (ninja, freetype, spirv-cross)
if command -v brew &> /dev/null; then
    MISSING_PKGS=""
    if ! command -v ninja &> /dev/null; then MISSING_PKGS="$MISSING_PKGS ninja"; fi
    if ! brew list freetype &>/dev/null; then MISSING_PKGS="$MISSING_PKGS freetype"; fi
    if ! brew list spirv-cross &>/dev/null; then MISSING_PKGS="$MISSING_PKGS spirv-cross"; fi
    if [ -n "$MISSING_PKGS" ]; then
        echo "[*] Installing required dependencies via Homebrew:$MISSING_PKGS..."
        brew install $MISSING_PKGS
    fi
fi

# 4. Locate Game Assets Directory
if [ -z "$GOW3_DIR" ]; then
    if [ -d "$SCRIPT_DIR/gow3_cusa01623_extracted" ]; then
        export GOW3_DIR="$SCRIPT_DIR/gow3_cusa01623_extracted"
    elif [ -d "$SCRIPT_DIR/../gow3_cusa01623_extracted" ]; then
        export GOW3_DIR="$SCRIPT_DIR/../gow3_cusa01623_extracted"
    elif [ -d "$HOME/gow3_cusa01623_extracted" ]; then
        export GOW3_DIR="$HOME/gow3_cusa01623_extracted"
    elif [ -d "/Volumes/1TB/gow3_cusa01623_extracted" ]; then
        export GOW3_DIR="/Volumes/1TB/gow3_cusa01623_extracted"
    elif [ -d "$SCRIPT_DIR/game" ]; then
        export GOW3_DIR="$SCRIPT_DIR/game"
    else
        echo "[!] Notice: GOW3_DIR not set. Will search in current directory."
    fi
fi

if [ -n "$GOW3_DIR" ]; then
    echo "[+] Game asset directory: $GOW3_DIR"
fi

# 5. Check guest_image.bin
if [ ! -f "$SCRIPT_DIR/guest_image.bin" ]; then
    echo "[!] Error: guest_image.bin not found in $SCRIPT_DIR"
    echo "    Make sure guest_image.bin is copied alongside this script."
    exit 1
fi

# 6. Configure build.ninja for Apple Silicon macOS (Idempotent)
if [ -f "$SCRIPT_DIR/build.ninja" ]; then
    if command -v python3 &>/dev/null; then
        python3 "$SCRIPT_DIR/tools/patch_build_ninja.py" "$SCRIPT_DIR/build.ninja"
    elif command -v python &>/dev/null; then
        python "$SCRIPT_DIR/tools/patch_build_ninja.py" "$SCRIPT_DIR/build.ninja"
    fi
fi

# 7. Build native macOS executable
echo "[*] Compiling native Apple Silicon runtime and linking ps4_app..."
if command -v ninja &> /dev/null; then
    ninja ps4_app
else
    echo "[*] Running fallback parallel build..."
    CFLAGS="-target arm64-apple-darwin -mcpu=apple-m2 -DRECOMP_ARM64_ASM=1 -O2 -ffp-contract=off -fvisibility=hidden -Wno-parentheses-equality -I. -I3rdparty/spirv-cross -I3rdparty/libatrac9/C/src -I3rdparty/minimp3 -I/opt/homebrew/include -I/opt/homebrew/include/spirv_cross -I/opt/homebrew/opt/freetype/include/freetype2 -I/opt/homebrew/include/freetype2 -Iruntime/core -Iruntime/kernel -Iruntime/kernel/aio -Iruntime/kernel/equeue -Iruntime/kernel/memory -Iruntime/kernel/sync -Iruntime/kernel/syscalls -Iruntime/kernel/threading -Iruntime/kernel/vfs -Iruntime/modules -Iruntime/modules/libSceAjm -Iruntime/modules/libSceAppContent -Iruntime/modules/libSceAudioIn -Iruntime/modules/libSceAudioOut -Iruntime/modules/libSceAudiodec -Iruntime/modules/libSceCamera -Iruntime/modules/libSceErrorDialog -Iruntime/modules/libSceFiber -Iruntime/modules/libSceFreeType -Iruntime/modules/libSceGnmDriver -Iruntime/modules/libSceHmd -Iruntime/modules/libSceHttp -Iruntime/modules/libSceIme -Iruntime/modules/libSceImeDialog -Iruntime/modules/libSceInvitationDialog -Iruntime/modules/libSceJpegDec -Iruntime/modules/libSceJpegEnc -Iruntime/modules/libSceJson2 -Iruntime/modules/libSceMouse -Iruntime/modules/libSceMove -Iruntime/modules/libSceMsgDialog -Iruntime/modules/libSceNet -Iruntime/modules/libSceNetCtl -Iruntime/modules/libSceNoopStubs -Iruntime/modules/libSceNp -Iruntime/modules/libSceNpProfileDialog -Iruntime/modules/libSceNpSns -Iruntime/modules/libSceNpTrophy -Iruntime/modules/libSceNpUtility -Iruntime/modules/libScePad -Iruntime/modules/libScePlayGo -Iruntime/modules/libScePngDec -Iruntime/modules/libScePosix -Iruntime/modules/libSceRandom -Iruntime/modules/libSceRemoteplay -Iruntime/modules/libSceRtc -Iruntime/modules/libSceSaveData -Iruntime/modules/libSceSaveDataDialog -Iruntime/modules/libSceScreenShot -Iruntime/modules/libSceSharePlay -Iruntime/modules/libSceSigninDialog -Iruntime/modules/libSceSocialScreen -Iruntime/modules/libSceSsl -Iruntime/modules/libSceSystemService -Iruntime/modules/libSceUserService -Iruntime/modules/libSceVideoOut -Iruntime/modules/libSceVideoRecording -Iruntime/modules/libSceVoice -Iruntime/modules/libSceVoiceQoS -Iruntime/modules/libSceVrTracker -Iruntime/modules/libSceWebBrowserDialog -Iruntime/modules/libkernel"
    LDFLAGS="-target arm64-apple-darwin -mcpu=apple-m2 -Wl,-dead_strip -Wl,-x -framework Metal -framework Cocoa -framework QuartzCore -framework GameController -framework AudioToolbox -framework ImageIO -framework MetalFX -framework VideoToolbox -framework CoreMedia -framework CoreVideo -lz -lpthread -lm -L/opt/homebrew/lib -L/opt/homebrew/opt/freetype/lib -lfreetype -lspirv-cross-msl -lspirv-cross-core -lspirv-cross-glsl -lc++"
    
    clang $CFLAGS -c main.c -o main.o
    for f in 3rdparty/libatrac9/C/src/*.c; do
        clang $CFLAGS -c "$f" -o "${f%.c}.o" &
    done
    find runtime -name "*.c" | while read -r f; do
        clang $CFLAGS -c "$f" -o "${f%.c}.o" &
    done
    find runtime -name "*.m" | while read -r f; do
        clang $CFLAGS -fobjc-arc -c "$f" -o "${f%.m}.o" &
    done
    find runtime -name "*.mm" | while read -r f; do
        clang++ $CFLAGS -fobjc-arc -std=c++17 -c "$f" -o "${f%.mm}.o" &
    done
    find gpu -name "*.cpp" | while read -r f; do
        clang++ $CFLAGS -std=c++17 -c "$f" -o "${f%.cpp}.o" &
    done
    wait
    
    echo "[*] Linking final executable ps4_app..."
    ALL_OBJS=$(find . -name "*.o" ! -name "test_reg.o")
    clang $ALL_OBJS $LDFLAGS -o ps4_app
fi

echo "[+] Build complete: ps4_app generated successfully!"
echo "[*] Launching God of War III Remastered on Apple Silicon..."
chmod +x ./ps4_app
./ps4_app
