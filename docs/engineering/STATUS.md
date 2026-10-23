# Engineering status

Updated: 2026-10-23. Current worktree: `luna/gow3-runtime-integration` at `/Users/ihabfawaqa/Downloads/gow3-arm64-runtime-int`. Production remains `luna/operation-spartan`; the candidate backend has not been installed in that application.

## Current state

| Area | Status | Evidence |
|---|---|---|
| Highest verified graphics milestone | **G2 partial** | Latest run `run_20261023_202535_550108` visibly confirms Sony at sample 120 and Santa Monica Studio through 1410, then black at 1440, a severely stretched Kratos face texture at 1590, and persistent white/black/white bands from 1650 through 5610. No correct game image is verified. |
| Active production renderer | **native Metal** | Latest primary-project run `run_20261023_202535_550108` logged `native-metal` on Apple M2. The GoW3 Vulkan library was not loaded by the game. |
| Latest bounded game run | **PARTIAL** | Run `run_20261023_202535_550108`: build PASS in 0.514 seconds; ARM64; reached the 60.872-second deadline without a crash; 188 internal frames, 95 presents, and 128 partial GPU snapshots. All frames, selected screenshots, trace files, and logs were copied to `/Users/ihabfawaqa/Downloads/GOW3-run-20261023-202535/`. A separate 45.269-second `GOW3_GCN=1` diagnostic remained mostly black and reported three fragment-shader pairs falling back because MSAA textures are unsupported. |
| Source-driven build graph | **PASS** | `tests/test_build_system_idempotence.py` passes; duplicate outputs are rejected; two graph generations are byte-identical. |
| Candidate application build | **PASS** | The isolated `ninja -j4 ps4_app` built the new marshalling object and linked a Mach-O arm64 executable; `file` and `lipo -archs` both report arm64. The standalone adapter smoke also compiles under `-Wall -Wextra -Werror`. |
| GoW3 GPU import | **PASS, standalone** | Pinned source `b907d8a9f2324ba5f514fbfe7de803e027c6ca26`, shadPS4 base `b4e7ae776071ceb1dd35004577a8719481e1d258`, full native arm64 CMake build, exports, and ABI smokes verified. GoW3 alias, depth/stencil, and blend CPU regression tests pass. |
| KosmicKrisp Vulkan | **PASS, bounded** | M2 feature/workload probe passes tested source-derived requirements. The latest standalone smoke initializes the candidate, resolves GNM NIDs, marshals a synthetic `GuestContext` NOP into the real candidate HLE, and waits for full `work_retired` before releasing command bytes. This is not the live app shim or a game workload. |
| MoltenVK | **PARTIAL / incompatible for renderer** | Diagnostic workloads work, but strict device creation fails on missing `robustBufferAccess2` and `nullDescriptor`. |
| Runtime guest-memory adapter | **PARTIAL** | Synthetic tests cover bounds, allocated/free VM extents, sparse holes, atomics, writeback, map/unmap/invalidation, runtime callbacks, host page protection, and 16 KiB M2 page granularity. The game does not install this API. |
| GNM dispatch / VideoOut | **NOT INTEGRATED** | The isolated typed marshaller reaches the real candidate submit HLE in a synthetic renderer smoke. `ps4_app` still dispatches real GNM imports to custom Metal. Candidate and primary VideoOut/equeue ownership remains unresolved. |

## Gate summary

| Gate | Status | Next evidence needed |
|---|---|---|
| M0 protected baseline and deterministic ARM64 build | **PASS** | Candidate clean and incremental builds plus two-generation graph test pass. |
| M1 automated splash/transition capture | **PARTIAL** | Both opening images and the first bad samples are saved; the bounded 1590/1650 draw traces lack matching per-draw texture/resource snapshots, so the producer remains unidentified. |
| M2 standalone pinned GoW3 GPU | **PASS, limited lifecycle** | Native library, required GoW3 regressions, ABI/init, and candidate raw-HLE PM4 NOP plus queue-drain smoke pass. Teardown and game GuestContext imports are not tested. |
| M3 Vulkan feature/workload probe | **PASS, bounded** | KosmicKrisp passes the source-derived tested workload; guest depth semantics and full game rendering remain open. |
| M4 real guest submissions through GoW3 GPU | **PARTIAL** | A synthetic `GuestContext` NOP now reaches the real candidate HLE and Liverpool queue on KosmicKrisp. The application import shim still does not call the candidate, so no real game submission has reached Liverpool. The first smoke exposed early buffer release before `work_retired`; a local wait/notification fix makes the bounded rerun pass. |
| M5 guest memory, faults, EOP/events | **PARTIAL** | Synthetic adapter passes; install it in the process and verify a live protected-page fault, completion label, event, and waiter. |
| M6 shader/texture validation | **NOT TESTED** | Capture actual guest shader and descriptors through the candidate backend and validate their contents. |
| M7 deterministic replay | **NOT TESTED** | No complete guest submission capture exists. |
| M8–M9 geometry, textures, multipass | **NOT TESTED** | Require a replayable guest workload and attachment evidence. |
| M10 recognizable post-splash image | **FAIL** | Latest production capture is black after the logos, followed by a distorted Kratos face texture and persistent white/black/white bands; no correct scene is verified. |
| M11–M14 audio/video, menu/gameplay, sustained performance | **NOT TESTED** | Depend on correct guest rendering and real input/presentation integration. |

Overall: **PARTIAL**. Standalone Vulkan/device success and operation counters do not pass M4 or establish game graphics correctness.
