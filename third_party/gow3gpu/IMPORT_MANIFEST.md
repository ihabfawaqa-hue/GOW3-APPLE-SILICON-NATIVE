# GoW3 GPU source and Apple Silicon port manifest

Updated: 2026-10-23. This is an isolated candidate import. It is not part of the production `ps4_app` link.

## Source provenance

| Item | Recorded value |
|---|---|
| Repository | `https://github.com/Sohebdsa/gow3_pc` |
| Branch | `gow3` |
| Repository revision | `b907d8a9f2324ba5f514fbfe7de803e027c6ca26` |
| shadPS4 base in `gpu/VENDOR.txt` | `b4e7ae776071ceb1dd35004577a8719481e1d258` |
| License | GPL-2.0-or-later; preserve all source headers and bundled license notices |
| Candidate architecture | Liverpool PM4, GCN shader frontend/IR/SPIR-V, Vulkan resource/cache/rendering, guest-facing GNM and VideoOut shims |
| Target checked | macOS 27.0, Apple M2, native arm64 |

The source copy includes the candidate `gpu/`, `src/`, tests, license, and readme. The existing `gpu/shadps4-recompiler/` is a different vendor snapshot and was not substituted for the full video core.

## Required GoW3 fixes

Each fix is an ancestor of the imported revision. Each accompanying CPU-side regression test was rebuilt and rerun with Apple clang as native arm64 after the local port changes.

| Commit | Fix | Test result |
|---|---|---|
| `036d5d7d78f5fef7b88ffa69b4f07bee108f697f` | Synchronize multiple compatible image interpretations backed by aliased guest memory | `tests/test_aliasing.cpp`: PASS |
| `4676e1c47171f1b1f255d70de3a5f8f07479770f` | Use effective depth/stencil attachment state for draws, including fullscreen/post-processing passes | `tests/test_depth_stencil_state.cpp`: PASS |
| `9c88add1e69657c87f215628573633a0529fa9a9` | Preserve GoW3 scaled MIN/MAX blending behavior and supplemental draw | `tests/test_blend_rewrite.cpp`: PASS |

These tests check CPU-side policy. They do not validate Vulkan image memory, guest GPU work, or game output.

`gpu/VENDOR.txt` additionally records shader-recompiler backports `233562929`, `d96278e1e`, `e518a6515`, `282e5c792`, `fb6367d29`, `94e217781`, `d1cb90dbb`, `dd43d1cac`, and `ef08b1e1f`. Those abbreviated upstream identities remain recorded as upstream supplied them; this manifest does not claim to have independently fetched or expanded each hash.

## Dependency pins

The upstream CMake refers to Git submodules. Required GPU submodules are real parent-repository gitlinks at the exact revisions below; `third_party/gow3gpu/.gitmodules` preserves the candidate repository's original submodule declarations. The outer worktree `.gitmodules` records their relocated paths under `third_party/gow3gpu/gpu/`.

| Dependency | Revision | License/source note |
|---|---|---|
| FSR Vulkan | `c64f093404125e960813f95f1996c14780f0dd69` | Upstream submodule `src/video_core/renderer_vulkan/fsr`; retain included license |
| Dear ImGui | `f1cc2ae15e53a861a874c3034aae6798fde194ab` | Upstream submodule `gpu/third_party/imgui`; retain included license |
| magic_enum | `e046b69a3736d314fad813e159b1c192eaef92cd` | Upstream submodule `gpu/third_party/magic_enum`; MIT |
| miniz | `2948eebafc5b9f1c69bebed80333e403a09bd62d` | Upstream submodule `gpu/third_party/miniz`; MIT |
| xbyak | `cd6bad3cad2e07a3e277c23d2b1f065320024717` | Upstream submodule `gpu/third_party/xbyak`; BSD-3-Clause |
| Vulkan Memory Allocator | `1d8f600fd424278486eade7ed3e877c99f0846b1` (`v3.3.0`) | Separate official import; header SHA-256 `90ce12fc4a2466235a09ae02905dd0c13aee80c1bbf11b331ab61230c2ceb112`; MIT |

`Sirit` and FSR3/FSR4 sources are included in the candidate GPU snapshot. System build dependencies used for this experiment were Homebrew fmt 12.2.0, Zydis 4.1.1, Zycore 1.5.2, Vulkan loader 1.4.0, xxhash 0.8.4, SDL3 3.4.18, FFmpeg 9.0.2, glslang 16.6.0, Boost 1.92, and robin-map 1.4.1. These are host build inputs, not redistributable project vendoring.

## Local macOS/ARM64 patch record

These are local port patches in the isolated branch, not upstream commits. They leave Liverpool packet, shader-recompiler, texture-cache, image-alias, depth/stencil, and blend algorithms unchanged.

| Area | Local adaptation |
|---|---|
| CMake/platform | Avoid Linux X11 discovery on Apple; search the pinned VMA header; accommodate Homebrew's imported `Zycore::Zycore` target; use Mach-O dynamic lookup for host callbacks |
| CPU relax/timing | Add `common/host_timing.h`; replace x86 pause/TSC assumptions in the affected Vulkan worker paths with an ARM-compatible relax hint and monotonic nanoseconds |
| Host thread identity | Add `common/thread_id.h`; avoid Linux/Windows-only thread ID APIs in Liverpool logging and rasterizer checks |
| Runtime logging/threads | Use Mach-O-safe thread names, Apple CPU-count/QoS APIs, and ARM64 ucontext register extraction in the guest-write logger |
| Embedded overlay font | Use Mach-O section naming and leading-underscore assembly symbols |
| PM4 address decode | Select pointer vs integer conversion at compile time instead of forming an invalid pointer cast on arm64 |
| Vulkan portability | On Apple, request `VK_KHR_portability_enumeration` and its instance flag when advertised; enable `VK_KHR_portability_subset` at device creation when the device advertises it |

No local workaround removes a renderer-required robustness feature. The M2 probe still rejects the source's required device feature set.

## Build and ABI evidence

| Check | Result |
|---|---|
| Full standalone `gow3gpu` CMake target | PASS: all target translation units compiled and the shared library linked |
| Library architecture | PASS: `/tmp/gow3_gpu_import_cmake/libgow3gpu.dylib` is Mach-O arm64 |
| Expected C exports | PASS: `gow3gpu_init`, `gow3gpu_register_kernel`, `gow3gpu_resolve`, `gow3gpu_handle_fault`, and `gow3gpu_symbol_count` are exported |
| ABI registration smoke | PASS: test loader registered 43 kernel/AVPlayer symbols, resolved `D0OdFMjp46I` (`sceKernelCreateEqueue`), and returned zero for an unknown NID |
| GPU initialization/teardown | NOT TESTED: `gow3gpu_init` immediately creates a Vulkan presenter through GnmDriver registration; the M2 lacks required features. The current C ABI has no shutdown function, and its window thread calls `_Exit(0)` when the window closes. |
| Runtime callback contract | BLOCKED: `nm -u` shows 14 unresolved names: `runtime_disabled_optimizations`, `runtime_errno`, `runtime_fault_recover`, `runtime_file_translate`, `runtime_memory_clamp`, `runtime_memory_gpu_protect`, `runtime_memory_region`, `runtime_memory_set_gpu_hooks`, `runtime_memory_write_backing`, `runtime_process_time_counter`, `runtime_process_time_us`, `runtime_restart`, `runtime_thread_attach_host`, and `runtime_tsc_frequency`. The primary runtime does not currently export this contract. ABI smoke supplies test-only stubs and is not a guest-memory test. |
| Production link/real GNM submission | NOT TESTED: this branch has not replaced the placeholder bridge or active Metal backend in `ps4_app`. |

The test-only callback stubs live in `scripts/lab/probes/gow3gpu/runtime_contract_stubs.cpp`; they must never be linked into the application.

## Vulkan probe result on the M2

The source-derived probe is `scripts/lab/probes/vulkan/`; its README records invocation and interpretation. The measured comparison driver is MoltenVK 1.4.2 on Apple M2, Vulkan API 1.3.357, driver version 0.2.2210. KosmicKrisp is not installed. The verified LunarG SDK archive installer previously crashed in its macOS accessibility bridge before component selection; license acceptance was not bypassed.

| Capability/workload | MoltenVK result |
|---|---|
| Unpatched pinned instance setup | FAIL: `vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER` because portability enumeration is not requested |
| Local Apple portability-enumeration + subset-extension configuration | PASS: M2 is enumerated and portability subset is enabled |
| Required device extensions | PASS: swapchain, push descriptor, vertex attribute divisor, robustness2 are advertised |
| Required features | FAIL: core `robustBufferAccess=true`, but `robustBufferAccess2=false`, `nullDescriptor=false`; `robustImageAccess2=true`, divisor feature true. The source asserts and enables the separate robustness2 features and emits null resource descriptors. |
| Strict pinned required-feature device creation | FAIL: `VK_ERROR_FEATURE_NOT_PRESENT` |
| Compatibility diagnostic device | Created only with feature bits filtered to reported support; not a renderer-compatible device |
| Swapchain acquire/submit/present | PASS |
| Push-descriptor compute, storage-buffer and storage-image write/readback | PASS |
| Offscreen clear, barrier, fence, mapped readback | PASS; pixel `[64,128,191,255]` |
| Shared-allocation image alias plus UNORM/sRGB view reinterpretation | PASS in the diagnostic workload |
| D16_UNORM_S8_UINT and D24_UNORM_S8_UINT image formats | Unsupported by the probed device |
| D32_SFLOAT_S8_UINT image format | Supported. The source falls back from D16S8 to D32S8 when D24S8 is also unavailable; guest depth interpretation and readback semantics remain unverified. |
| Aggregate verdict | PARTIAL; process exits 1 because the real renderer's required feature/device creation gate fails |

The passing diagnostic workloads establish useful host capabilities but do not validate the pinned renderer or any PS4 workload. The report is machine-readable at `/tmp/gow3-moltenvk-probe.json`; it and game-derived captures remain outside Git.

## Primary runtime boundary audit

The primary runtime allocates a 512 GiB region with `mmap(NULL, ...)` and represents a guest VA as an offset into `GuestContext.mem_base` (`runtime/core/recomp_runtime.c`). For example, the active GNM shim translates its argument arrays with `mem_base + guest_offset`. The imported GPU does not share this representation: `Core::Memory::CopySparseSerial` queries a range using the guest VA and then copies from `reinterpret_cast<const void*>(source)`. Other direct VA casts remain in the imported texture cache, buffer cache, and rasterizer, including direct image write-back, index data hashing/fetch, and ring-constant reads. Passing the 14 callbacks or mapping the same numeric address through `runtime_memory_region` cannot make those values valid host pointers.

The import dispatch is also incompatible without an adapter. The primary `guest_modules.c` registry invokes `shim_sceGnmSubmitCommandBuffers(GuestContext*)`; that shim decodes guest register arguments and calls the local Metal implementation. The imported `gow3gpu_resolve(const char *scoped_nid)` returns direct PS4_SYSV HLE function pointers and is not called anywhere in the primary tree. It cannot be installed as a `GuestContext*` shim by pointer cast. A safe integration therefore needs both (1) a checked guest-VA translation/read/write API, applied to every guest-address dereference in the imported renderer, and (2) import dispatch that adapts the primary shim calling convention to the actual imported HLE ABI.
