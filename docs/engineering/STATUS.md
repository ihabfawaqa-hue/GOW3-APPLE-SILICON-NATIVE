# Engineering status

Updated: 2026-10-23. Working branch: `luna/operation-spartan`. The original source tree had no Git metadata; local baseline commit `f786687` was created after excluding the guest image, generated game code, binaries, run captures, and backups. No remote is configured.

## Current state

| Area | Status | Evidence |
|---|---|---|
| Highest graphics milestone | **G2 user-reported; not automated** | The user reports the Sony and Santa Monica splashes display correctly. Existing captured frames after startup are corrupted. |
| Active production renderer | **native Metal** | `sceGnmSubmitCommandBuffers` directly calls `ps4_gnm_metal_process_command_buffers`; VideoOut presents through `ps4_gnm_metal_present`. A prior local run logged an Apple M2 Metal device. |
| GoW3 GPU bridge | **compiled, inactive placeholder** | `gow3gpu_bridge.o` is linked but no runtime source calls `gow3gpu_init`, `gow3gpu_submit_command_buffers`, or `gow3gpu_present`; the bridge forwards back to Metal. |
| Shader path | **partial, opt-in Metal translation** | Vendored shadPS4 shader recompiler builds into the app; `GOW3_GCN=1` translates selected shaders to MSL and falls back on unsupported draws. This is not the shadPS4 Vulkan video core. |
| Vulkan backend | **not present / not tested** | The binary has no Vulkan loader dependency. This host has Vulkan headers but no `vulkaninfo`, Vulkan loader, KosmicKrisp, or MoltenVK installation detected. |
| Build graph | **PARTIAL** | Two consecutive graph generations matched (SHA-256 `7a844db1e8d091d830eba76a6bcf61fb083a8300d8a4430cb5049e80a816066b`). `ninja -j4 ps4_app` linked successfully and `file`/`lipo` report arm64. Clean-tree and source-edit incremental checks remain. |
| Test laboratory | **PARTIAL** | Bounded `smoke` completed at its 45-second deadline. It captured 42 internal frames and partial draw/resource snapshots under `~/Library/Application Support/GOW3Lab/runs/run_20261023_120623_074376`. |
| GoW3 GPU source | **PARTIAL** | Inspected upstream `Sohebdsa/gow3_pc` at `b907d8a9f2324ba5f514fbfe7de803e027c6ca26`; all three required fix commits and regression tests are present. Their CPU-side aliasing, blend, and depth/stencil tests pass as arm64 executables. The full GPU source is not yet imported or built. |
| Upstream GPU configure | **BLOCKED** | Native CMake configure stops at `Could NOT find Vulkan (missing: Vulkan_LIBRARY)`; the host has Vulkan headers but no loader or driver. The pinned project's non-Windows CMake path also requests X11, so macOS build rules need a deliberate adaptation. |
| Recognizable post-splash frame | **NOT ACHIEVED** | The latest internal capture is a measured all-black 1920×1080 frame; no capture is accepted as recognizable GoW3 imagery. |

## Gate summary

| Gate | Status | Next evidence needed |
|---|---|---|
| M0 protected baseline and deterministic ARM64 builds | **PARTIAL** | Graph generation is deterministic and link/architecture pass. Clean worktree and source-edit incremental build remain. |
| M1 splash and first-broken-frame regression | **PARTIAL** | The smoke lab runs and captures internally, but this run did not verify the user-reported splashes and ended on black. |
| M2 standalone GoW3/shadPS4 GPU build | **PARTIAL** | Three upstream pure-CPU GPU-state regression tests pass natively. Full video-core target is blocked at Vulkan loader discovery. |
| M3 Vulkan feature/workload probe | **BLOCKED** | No SDK loader, KosmicKrisp, or MoltenVK is installed; upstream CMake confirms the missing Vulkan library. |
| M4 guest GPU submissions through shadPS4 | **FAIL** | The live call graph currently reaches custom Metal only. |
| M5–M14 | **NOT TESTED** | Depends on GPU backend integration and correct graphics. |

Overall: **PARTIAL**. No graphics gate above G2 has been passed by build activity, draw counts, or CPU-side unit tests alone. The smoke run's backend was native Metal, not the GoW3 GPU.
