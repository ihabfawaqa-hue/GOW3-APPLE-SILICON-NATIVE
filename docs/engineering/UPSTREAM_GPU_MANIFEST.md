# Upstream GPU import manifest

Updated: 2026-10-24. The current isolated import is `/Users/ihabfawaqa/Downloads/gow3-arm64-runtime-int`, branch `luna/gow3-runtime-integration`, commit `fe69130` (synthetic submit/queue-retirement fix `a0a7a9d`). The production `ps4_app` still does not load or link this candidate GPU; it continues to use the custom Metal path.

## Source and provenance

| Item | Value |
|---|---|
| Upstream repository | `https://github.com/Sohebdsa/gow3_pc` |
| Inspected branch / revision | `gow3` / `b907d8a9f2324ba5f514fbfe7de803e027c6ca26` |
| Imported source | Candidate `gpu/`, `src/`, tests, licenses, and exact Git submodules under `third_party/gow3gpu/` |
| Vendored shadPS4 base | `b4e7ae776071ceb1dd35004577a8719481e1d258`, per upstream `gpu/VENDOR.txt` |
| License | GPL-2.0-or-later GPU source; per-file SPDX notices and bundled dependency licenses are retained |
| Upstream build | CMake, C++23 shared library; dependencies and revisions are recorded in `third_party/gow3gpu/IMPORT_MANIFEST.md` |
| Tested target | macOS 27.0, Apple M2, native arm64 |

`gpu/VENDOR.txt` records shader-recompiler backports `233562929`, `d96278e1e`, `e518a6515`, `282e5c792`, `fb6367d29`, `94e217781`, `d1cb90dbb`, `dd43d1cac`, and `ef08b1e1f`. The manifest preserves the upstream abbreviated identifiers; it does not claim to have expanded or independently verified each commit hash. Do not substitute arbitrary current shadPS4 files.

## GoW3-specific corrections

All three requested commits are present in the selected source revision. Their CPU-side regression programs were rebuilt and passed as native arm64 executables:

| Commit | Correction | Regression |
|---|---|---|
| `036d5d7d78f5fef7b88ffa69b4f07bee108f697f` | Synchronize compatible image views backed by aliased guest memory | `test_aliasing.cpp`: PASS |
| `4676e1c47171f1b1f255d70de3a5f8f07479770f` | Use effective depth/stencil attachment state, including fullscreen/post-processing draws | `test_depth_stencil_state.cpp`: PASS |
| `9c88add1e69657c87f215628573633a0529fa9a9` | Preserve scaled MIN/MAX blending behavior, including the GoW3 supplemental draw | `test_blend_rewrite.cpp`: PASS |

These CPU-side tests do not validate actual Vulkan image memory, guest command processing, or game output.

## Local Apple Silicon port and build evidence

The local platform changes are committed in the isolated branch and itemized in `third_party/gow3gpu/IMPORT_MANIFEST.md`. They cover macOS CMake/windowing and Vulkan portability setup, ARM64 timing/thread helpers, Mach-O symbols, page tracking at the M2's 16 KiB granularity, versioned guest-memory and host-runtime callbacks, and PM4 buffer lifetime through Liverpool `work_retired`. The source ownership, patches, dependencies, licenses, and test commands are recorded there.

| Check | Result |
|---|---|
| Full candidate GPU CMake target | PASS: `libgow3gpu.dylib` builds as Mach-O arm64 at `/tmp/gow3gpu-runtime-int-build/libgow3gpu.dylib` |
| Guest-memory contract and host-service callbacks | PASS, synthetic: versioned callbacks and host runtime tests pass; `nm -u` shows no unresolved project `runtime_*` callback imports |
| GNM `GuestContext` submit adapter | PASS, synthetic: validates guest argument arrays, copies bounded DCB/CCB bytes, retains them through submit-done and full retirement, rejects an unmapped array |
| Candidate renderer and queue smoke | PASS, limited: KosmicKrisp initializes the candidate; a synthetic `GuestContext` PM4 NOP reaches the real candidate HLE and Liverpool queue, which reports `work_retired` before buffer release |
| Candidate GPU teardown | NOT TESTED: the candidate C ABI still lacks coordinated renderer, kernel-service, SDL/window, and main-queue timer shutdown |
| Production integration | NOT TESTED: no real game GNM import, guest submission, game memory, EOP/equeue event, or integrated VideoOut reaches this library |

The candidate-specific NOP smoke first failed because submit-done returned before draw-pipe/deferred-signal retirement. The isolated fix makes `gow3gpu_wait_idle` wait for `work_retired`, and the bounded rerun passes. This diagnostic wait is not a substitute for the guest's EOP/equeue completion contract. The separate active Metal path was subsequently fixed to wait for its submitted Metal command buffer before publishing labels and events. A 25-second PS4 Pro game smoke sampled completion through total 6,240 with no rejection logs. That result validates only the legacy comparison path and does not satisfy the candidate's completion contract.

## Vulkan evidence on Apple M2

KosmicKrisp, built from Mesa revision `09a9fae26d5df276a23b3f66050cd0d1d227800a`, passes the tested source-derived feature/device creation gate, swapchain presentation, storage-image compute/readback, offscreen barriers/readback, image-allocation aliasing, and view reinterpretation. The standalone presenter also initializes. The report and build logs are under `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/vulkan_20261023_kosmickrisp_m2/`.

D16S8 and D24S8 are unsupported by the probed device; D32S8 is supported. Guest depth conversion, sampling, stencil behavior, and readback through that fallback are not tested. This is a bounded host capability pass, not a GoW3 rendering pass.

MoltenVK 1.4.2 passes diagnostic presentation and readback cases but fails strict pinned renderer device creation because `robustBufferAccess2` and `nullDescriptor` are missing. No feature-bit bypass is accepted. Its report is `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/vulkan_20261023_moltenvk_m2/vulkan_moltenvk.json`.

## Production boundary and latest game evidence

The primary call graph still routes `shim_sceGnmSubmitCommandBuffers(GuestContext*)` to `ps4_gnm_metal_process_command_buffers`; `ps4_gnm_metal_present` owns presentation. The placeholder `gpu/gow3gpu_bridge.cpp` forwards back to Metal and is not the imported backend. The candidate `gow3gpu_resolve(const char *scoped_nid)` returns PS4_SYSV HLE function addresses, so it cannot be cast to the primary runtime's `GuestContext*` shim. The submit-specific adapter has been exercised synthetically, but no application call site invokes it.

The latest bounded production run is `run_20261023_203917_961646`; it reports `native-metal`. The Sony card at sample 120 and Santa Monica card through 1440 are visually correct. White samples appear at 1470–1560, corrupted grayscale output appears at 1590, a stretched brown texture at 1620, a white frame at 1650, and a repeated black/white split from 1680 through 5610. Screenshots, copied logs, and selected draw traces are under `/Users/ihabfawaqa/Downloads/GOW3-run-20261023-203917/`. The frame trace does not contain the matching per-draw texture/resource snapshots needed to attribute the corruption. The optional custom Metal GCN translation path remains disabled in the production run.

Do not promote the candidate until the runtime installs its memory and host-service contracts, routes typed GNM imports, establishes one GPU completion/event owner and one VideoOut presenter, and passes a real bounded guest workload with safe teardown.
