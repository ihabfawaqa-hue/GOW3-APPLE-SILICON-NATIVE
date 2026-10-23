# Upstream GPU import manifest

Updated: 2026-10-23. This manifest records the source audited for a future import. It does **not** claim the full GPU library is part of the local application yet.

## Candidate source

| Field | Value |
|---|---|
| Repository | [Sohebdsa/gow3_pc](https://github.com/Sohebdsa/gow3_pc) |
| Branch inspected | `gow3` |
| Repository commit | `b907d8a9f2324ba5f514fbfe7de803e027c6ca26` |
| Imported path audited | `gpu/` and root `tests/` |
| Vendored shadPS4 base recorded by upstream | `b4e7ae776071ceb1dd35004577a8719481e1d258` |
| License | GPL-2.0-or-later for the GPU source; retain per-file SPDX and third-party license files |
| Upstream build configuration | CMake, C++23, position-independent code, shared `gow3gpu`; requires Vulkan, fmt, Boost, tsl-robin-map, VMA, xxhash, SDL3, X11 on all non-Windows platforms, Zydis, glslang, and included sirit/FSR Vulkan/miniz components |
| Tested target architecture | Three pure CPU regression tests compiled and run as macOS arm64; full Vulkan GPU target has not compiled |
| Local import status | Not imported into the application tree; current local `gpu/gow3gpu_bridge.cpp` remains a placeholder |

The full candidate `gpu/` is a substantial emulator subsystem: Vulkan Liverpool command processing, video core, shader recompiler, guest-facing shims, and associated libraries. It is not interchangeable with the local `gpu/shadps4-recompiler/` subset. Keep the two source histories separate until a reviewed integration selects exact files and patches.

## GoW3 corrections verified at the candidate commit

All three commits below are ancestors of `b907d8a9f2324ba5f514fbfe7de803e027c6ca26`; their source and corresponding tests were inspected. Each regression test passed when compiled with the native Apple clang toolchain as an arm64 executable.

| Commit | Correction | Regression test | Result |
|---|---|---|---|
| `4676e1c47171f1b1f255d70de3a5f8f07479770f` | Effective depth/stencil attachment state; omit attachments that cannot affect the draw | `tests/test_depth_stencil_state.cpp` | PASS |
| `036d5d7d78f5fef7b88ffa69b4f07bee108f697f` | Synchronize compatible image interpretations that alias the same guest memory | `tests/test_aliasing.cpp` | PASS |
| `9c88add1e69657c87f215628573633a0529fa9a9` | Rewrite scaled MIN/MAX blend behavior, including the GoW3 supplemental squared pass | `tests/test_blend_rewrite.cpp` | PASS |

Test outputs and commands are recorded in `TEST_MATRIX.md`. These CPU-side rules tests do not test Vulkan image copying, pipeline execution, or rendered game output.

The reproducible native commands (run from the upstream checkout root) were:

```sh
clang++ -std=c++23 -O0 -UNDEBUG -I gpu/shim -I gpu/shadps4 -I gpu/third_party/gcn/include -I /opt/homebrew/include \
  tests/test_aliasing.cpp gpu/shadps4/video_core/texture_cache/aliasing.cpp \
  gpu/shadps4/video_core/texture_cache/host_compatibility.cpp \
  -L/opt/homebrew/lib -lfmt -o /tmp/gow3_gpu_tests/aliasing-test
/tmp/gow3_gpu_tests/aliasing-test

clang++ -std=c++23 -O0 -UNDEBUG -I gpu/shim -I gpu/shadps4 -I gpu/third_party/gcn/include -I /opt/homebrew/include \
  tests/test_blend_rewrite.cpp -L/opt/homebrew/lib -lfmt \
  -o /tmp/gow3_gpu_tests/blend-rewrite-test
/tmp/gow3_gpu_tests/blend-rewrite-test

clang++ -std=c++23 -O0 -UNDEBUG -I gpu/shim -I gpu/shadps4 -I gpu/third_party/gcn/include -I /opt/homebrew/include \
  tests/test_depth_stencil_state.cpp -L/opt/homebrew/lib -lfmt \
  -o /tmp/gow3_gpu_tests/depth-stencil-state-test
/tmp/gow3_gpu_tests/depth-stencil-state-test
```

## Other upstream shader-recompiler backports

The candidate's `gpu/VENDOR.txt` additionally records these shadPS4 shader-recompiler backports after the base commit: `233562929`, `d96278e1e`, `e518a6515`, `282e5c792`, `fb6367d29`, `94e217781`, `d1cb90dbb`, `dd43d1cac`, and `ef08b1e1f`. Preserve the upstream manifest as source evidence; before import, expand each abbreviation to a full commit hash and verify its patch is present in the selected source.

The existing local `gpu/shadps4-recompiler/VENDOR.txt` is a separate vendoring record with its own shadPS4/Sirit/SPIRV-Headers revisions and ARM64 port patches. Do not overwrite it with the candidate `gpu/VENDOR.txt` or claim that its shader-only tree contains the Vulkan video core.

## Platform/configuration findings

The candidate uses C++23 and asks CMake for a shared library. Its `if (NOT WIN32) pkg_check_modules(X11 ...)` condition also includes Apple platforms, so the Linux window-system dependency must be corrected for macOS. The first native configure attempt stopped earlier at `find_package(Vulkan REQUIRED)` because the host has headers but no Vulkan loader library. No system package was installed during this inspection.

Before product import:

1. Create a dedicated worktree/branch for the source integration.
2. Retain the candidate commit and base shadPS4 commit in this manifest.
3. Add a macOS platform branch using the selected Vulkan loader and window/presentation path, without dropping renderer features.
4. Link only one GPU owner to the existing runtime and implement the candidate's actual C ABI from `gpu/gow3gpu.h`.
5. Record local changes as commits or patches here; do not silently replace vendored files.
6. Re-run these three regression tests after any source or build-system changes.

## Current ABI boundary observed

`gpu/gow3gpu.h` defines a C ABI that includes `gow3gpu_register_kernel()`, `gow3gpu_init(const Gow3GpuConfig*)`, `gow3gpu_resolve(const char*)`, `gow3gpu_handle_fault(void*, void*)`, `gow3gpu_dump_guest_writes(void*)`, text/choice input helpers, `gow3gpu_overlay_captures_input()`, and `gow3gpu_symbol_count()`. The exact struct layout and function behavior must be read from that header at integration time. Do not reuse the local placeholder ABI by name alone.
