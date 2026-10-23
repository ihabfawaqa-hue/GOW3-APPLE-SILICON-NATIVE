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
| Test target portability | Restrict the live cheat-hook executable test (Windows `VirtualAlloc` plus x86-64 machine code) to 64-bit Windows so it does not break the native macOS/arm64 default build |
| CPU relax/timing | Add `common/host_timing.h`; replace x86 pause/TSC assumptions in the affected Vulkan worker paths with an ARM-compatible relax hint and monotonic nanoseconds |
| Host thread identity | Add `common/thread_id.h`; avoid Linux/Windows-only thread ID APIs in Liverpool logging and rasterizer checks |
| Runtime logging/threads | Use Mach-O-safe thread names, Apple CPU-count/QoS APIs, and ARM64 ucontext register extraction in the guest-write logger |
| Embedded overlay font | Use Mach-O section naming and leading-underscore assembly symbols |
| PM4 address decode | Select pointer vs integer conversion at compile time instead of forming an invalid pointer cast on arm64 |
| Vulkan portability | On Apple, request `VK_KHR_portability_enumeration` and its instance flag when advertised; enable `VK_KHR_portability_subset` at device creation when the device advertises it |
| Cocoa presentation | Create the SDL Cocoa window and CAMetalLayer on the process main thread, pass the layer to the pinned `VK_EXT_metal_surface` path, and pump SDL events from a main-queue timer. Avoid SDL's Vulkan-window loader path so `VK_DRIVER_FILES` selects the renderer's Vulkan ICD. |
| Apple Silicon page tracking | Use 16 KiB GPU tracker pages on Darwin arm64 so candidate page protections match the M2 host VM page size; retain 4 KiB tracking on other targets. Verify the compiled granularity against `_SC_PAGESIZE`. |
| Empty instance-layer diagnostics | Treat a successful query with zero installed Vulkan layers as normal availability information, not a failed query. |
| Guest-memory boundary | Replace GPU-side `VAddr` host-pointer dereferences with versioned guest-VA callbacks for range checks, sparse reads, host-to-guest fault mapping, atomic PM4 waits/semaphores, write-back, protection, and mapping invalidation hooks. The primary adapter treats the image/argument prefix and non-free VM extents as mapped; free extents are sparse holes and cross-hole accesses fail. |
| Guest shader lifetime | Snapshot program bytes through the guest-memory service and retain shared ownership through cached `Shader::Info` and geometry pipeline selection |
| PM4 debug capture | Snapshot indirect command buffers through the checked guest-memory API, with a 64 MiB cap, rather than treating their guest VA as a host pointer |
| Host runtime boundary | Define versioned `Gow3GpuHostRuntimeApi` callbacks for host errno, process clocks, VFS translation, host-thread attach/detach and restart requests; keep renderer optimization state and recoverable-fault TLS inside the candidate library |
| Diagnostic queue drain | Export `gow3gpu_wait_idle` for bounded standalone probes to wait until Liverpool retires queued submissions; this is not used to emulate guest EOP or equeue semantics |
| Submission-buffer lifetime | Make `gow3gpu_wait_idle` wait for `work_retired`, including draw-pipe draining and deferred Vulkan signals, before the runtime adapter releases host-owned DCB/CCB copies |

No local workaround removes a renderer-required robustness feature. MoltenVK still rejects the source's required device feature set; the source-built KosmicKrisp ICD passes the source-derived requirement check and representative workloads.

## Build and ABI evidence

| Check | Result |
|---|---|
| Full standalone `gow3gpu` CMake target | PASS: all target translation units compiled and the shared library linked as Mach-O arm64 at `/tmp/gow3gpu-runtime-int-build/libgow3gpu.dylib` |
| Default CMake build on macOS/arm64 | PASS: after restricting the Windows/x86-64-only cheat-hook test to its supported platform, `cmake --build /tmp/gow3_gpu_import_cmake --parallel 4` completes |
| Library architecture | PASS: `/tmp/gow3_gpu_import_cmake/libgow3gpu.dylib` is Mach-O arm64 |
| Expected C exports | PASS: `gow3gpu_init`, diagnostic `gow3gpu_wait_idle`, `gow3gpu_register_kernel`, `gow3gpu_resolve`, `gow3gpu_handle_fault`, and `gow3gpu_symbol_count` are exported |
| ABI registration smoke | PASS: test loader registered 43 kernel/AVPlayer symbols, resolved `D0OdFMjp46I` (`sceKernelCreateEqueue`), and returned zero for an unknown NID |
| Guest-memory API unit test | PASS: `guest-memory-api-test` validates callback requirements and one-time install, guest mapping, reverse host-address lookup, acquire atomic reads, acquire-release semaphore updates, sparse reads, streaming hash equivalence, checked write-back, GPU protection/hooks, invalid-range rejection, and failure behavior; native arm64 executable exits 0 |
| Candidate memory callback imports | PASS for memory surface: `nm -u libgow3gpu.dylib` shows no `runtime_memory_*` imports; the tested v1 callback interface replaces that legacy contract in the candidate |
| Standalone renderer/GNM protocol smoke | PASS, limited: `gow3gpu_init` constructed the Cocoa `CAMetalLayer`, KosmicKrisp Vulkan instance/device, presenter, and GNM/VideoOut registrations on Apple M2. The smoke resolved the candidate submit/done NIDs, marshalled a synthetic `GuestContext` NOP through the real candidate HLE, and waited for full `work_retired` before releasing copied buffers. Guest memory/runtime callbacks are synthetic; it does not invoke the primary app's live shim or represent a game workload. Result and bounded logs: `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/gow3gpu_guest_context_20261023_m2_kosmickrisp_retirement/`. |
| Renderer teardown | NOT TESTED: the C ABI has no shutdown function and does not coordinate Presenter, kernel-service, dispatch-timer, and SDL destruction. The bounded smoke ends its own process after initialization. |
| Candidate host-runtime symbols | PASS: candidate-owned implementations resolve every `runtime_*` symbol internally; `nm -u` shows no undefined imports for the primary runtime callbacks. Synthetic callback-forwarding, host-thread attach/detach, restart, clock, VFS, errno, optimization-state, and recoverable-fault tests pass. |
| Primary runtime API adapter | PARTIAL: `runtime/core/gow3gpu_runtime_adapter.c` builds against the branch's `GuestContext`, VFS, clock and VM APIs. Synthetic tests reject free VM extents and ranges crossing into holes, accept static/allocated extents, and cover sparse reads, atomic waits/semaphores, writeback, host callbacks, map/unmap/remap/CPU-write invalidation, page protection, and fault dispatch. Logs and machine-readable results are under `~/Library/Application Support/GOW3Lab/runs/gow3gpu_memory_adapter_20261023_m2/`. No application call site has loaded the candidate dylib or installed these APIs yet. |
| M2 GPU page tracking | PASS for granularity: the renderer now tracks 16 KiB pages on Darwin arm64; the M2 host probe reports `_SC_PAGESIZE=16384` and tracker page size `16384`. This does not validate a guest page fault or cache invalidation workload. |
| Guest-memory adapter and live page faults | NOT TESTED: the candidate API adapter is compiled and its synthetic contract passes, but the runtime has not installed it into a game process. No protected guest page has faulted through the imported renderer during a real run. |
| Production link/real GNM submission | NOT TESTED: this branch has not replaced the placeholder bridge or active Metal backend in `ps4_app`. |

The test-only fault-recovery hook is included only when the candidate GPU is configured with `-DGOW3GPU_TEST_HOOKS=ON`; it is not part of the production library configuration. The old executable-level runtime stub source was removed.

## Vulkan probe results on the M2

The source-derived probe is `scripts/lab/probes/vulkan/`; its README records invocation and interpretation. Mesa/KosmicKrisp was built from Mesa revision `09a9fae26d5df276a23b3f66050cd0d1d227800a`, installed only under `/tmp`, and tested with `VK_DRIVER_FILES`. The Apple M2 / KosmicKrisp / Vulkan 1.4.363 probe passes renderer-required extensions and features, presentation, storage-image compute/readback, offscreen barriers/readback, and aliased-image reinterpretation. The standalone presenter initialization also passes. The report and build logs are local at `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/vulkan_20261023_kosmickrisp_m2/`; init logs are in the sibling `gow3gpu_init_20261023_m2_kosmickrisp/` run.

MoltenVK 1.4.2 remains a comparison result, not the selected driver. It passes diagnostic workloads but lacks `robustBufferAccess2` and `nullDescriptor`. No Vulkan SDK license flow was bypassed; the KosmicKrisp ICD was built from Mesa source.

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

KosmicKrisp API 1.4.363, driver 26.99.99, Apple M2:

| Capability/workload | KosmicKrisp result |
|---|---|
| Source-required extension and feature checks; strict source device creation | PASS |
| Swapchain acquire/submit/present | PASS |
| Push-descriptor compute, storage-image and host readback | PASS |
| Offscreen clear, barrier, fence, and readback | PASS |
| Shared-allocation image alias and view reinterpretation | PASS |
| D16S8 and D24S8 image formats | Unsupported; D32S8 is supported. Guest depth conversion/sampling/readback still needs a game-resource test. |
| Aggregate verdict | PASS for the tested source requirements and workloads; full guest rendering is NOT TESTED |

The passing diagnostic workloads establish useful host capabilities but do not validate the pinned renderer or any PS4 workload. The report is machine-readable at `/tmp/gow3-moltenvk-probe.json`; it and game-derived captures remain outside Git.

## Primary runtime boundary audit

The primary runtime allocates a 512 GiB region with `mmap(NULL, ...)` and represents a guest VA as an offset into `GuestContext.mem_base` (`runtime/core/recomp_runtime.c`). For example, the active GNM shim translates its argument arrays with `mem_base + guest_offset`. The isolated candidate now uses a versioned guest-VA memory service for the audited GPU reads/writes, shader and resource snapshots, PM4 waits and semaphore atomics, and write-back. The new primary adapter uses that same `mem_base`, follows dynamic VM extents and direct-memory remaps, and has synthetic contract coverage; an address-cast audit found no remaining `VAddr` dereferences in the candidate's audited GPU paths. Candidate `runtime_*` symbols are served through a separate versioned host callback table. The primary adapter has not yet been installed in `ps4_app`, and no live GNM submission reaches the candidate.

The import dispatch is still incompatible without an adapter. The primary `guest_modules.c` registry invokes `shim_sceGnmSubmitCommandBuffers(GuestContext*)`; that shim decodes guest register arguments and calls the local Metal implementation. The imported `gow3gpu_resolve(const char *scoped_nid)` returns direct PS4_SYSV HLE function pointers and is not called anywhere in the primary tree. It cannot be installed as a `GuestContext*` shim by pointer cast. The next integration gate is an explicit per-import ABI adapter plus a single VideoOut owner; memory API conformance alone does not route GNM submissions.
