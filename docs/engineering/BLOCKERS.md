# Verified blockers

Updated: 2026-10-23.

| Severity | Blocker | Evidence | Next falsifiable action |
|---|---|---|---|
| Critical | The production GNM and VideoOut paths are custom Metal. | Direct calls in `runtime/modules/libSceGnmDriver/ps4_gnmdriver.c` and `runtime/modules/libSceVideoOut/ps4_videoout.c`; no caller of the GoW3 bridge. | Build the pinned GoW3/shadPS4 video core standalone, then route one real GNM import through it. |
| Critical | The current display after the opening sequence is corrupted. | Existing captures contain giant green polygons and black/white frames; earlier reports contain extreme geometry and texture artifacts. | Capture the exact first broken submission and inspect its state/resources; do not change geometry based on frame colors. |
| Critical | No Vulkan loader or candidate driver is installed. | Host has Vulkan headers 1.4.363.0 only. Native upstream CMake configure fails with `Could NOT find Vulkan (missing: Vulkan_LIBRARY)`. No Vulkan SDK folder was found. | Provision an isolated Vulkan SDK/runtime for feature tests, then run the source-derived probe against KosmicKrisp first and MoltenVK second. |
| High | LunarG SDK installer cannot advance with the current macOS accessibility client. | The verified 1.4.363.0 installer terminates with `EXC_BAD_ACCESS` in `QMacAccessibilityElement accessibilitySelectedChildren` while advancing to component selection; no files were installed. | Try a source/package-based runtime path or an installer version with a compatible UI; do not bypass component license acceptance. |
| High | The pinned upstream GPU CMake is not macOS-ready as-is. | `gpu/CMakeLists.txt` requires X11 for every non-Windows build, including macOS; the current macOS configure has not reached that check because Vulkan discovery fails first. | Adapt platform conditions in a private integration worktree and configure again without weakening renderer requirements. |
| High | Existing GPU captures are incomplete draw snapshots, not deterministic guest-GPU replay inputs. | The lab reports partial draw JSON and `PACKET_DECODE_ONLY_NO_GPU_RENDER`. | Add bounded rolling PM4/register/resource capture before designing full replay. |
| High | Shader/resource behavior is not yet trustworthy. | The captured smoke run selected native Metal, recorded 42 frames (32 black/no-frame and 10 flat clear/fallback), and its latest sampled frame was entirely black. It contains only a partial draw snapshot; replay is not implemented. | First prove full GPU initialization and one real submission through the vendored renderer; then preserve a bounded transition trace. |
| High | Full upstream GPU source is isolated but not linked into the application. | Exact candidate source is copied into `third_party/gow3gpu/` on this branch; local `gow3gpu_bridge.cpp` still forwards to Metal. | Build its standalone shared library and implement its real C ABI before replacing any runtime call path. |
| Medium | The prior project was not under version control. | No `.git` directory or parent worktree existed at the stated path. | Local baseline commit `f786687` now protects source history; local game-derived inputs and captures are excluded. |

No GPU correctness, menu, gameplay, audio, or cinematic acceptance result is currently claimed.
