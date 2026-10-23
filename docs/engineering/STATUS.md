# Engineering status

Updated: 2026-10-23. Current worktree: `luna/gow3-runtime-integration` at `/Users/ihabfawaqa/Downloads/gow3-arm64-runtime-int`. Production remains `luna/operation-spartan`; the candidate backend has not been installed in that application.

## Current state

| Area | Status | Evidence |
|---|---|---|
| Highest verified graphics milestone | **G2** | Internal captures visibly show the Sony opening card and Santa Monica Studio logo. The transition then produces an all-white sample and a half-black sample. No recognizable gameplay image is verified. |
| Active production renderer | **native Metal** | Latest primary-project run `run_20261023_194648_251517` logged `native-metal` on Apple M2. The GoW3 Vulkan library was not loaded by the game. |
| Latest bounded game run | **PARTIAL** | 45.387-second deadline; build PASS; ARM64; 44 internal frames; 44 presentations. Sample 840 is the last correct Santa Monica frame, sample 900 is the first sampled white frame, and sample 1200 is half-black. Image authenticity remains unverified. Logs and 44 screenshots are copied to `/Users/ihabfawaqa/Downloads/GOW3-run-20261023-194648/`. |
| Source-driven build graph | **PASS** | `tests/test_build_system_idempotence.py` passes; duplicate outputs are rejected; two graph generations are byte-identical. |
| Candidate application build | **PASS** | A clean isolated `ninja -j4 ps4_app` completed all 315 actions and linked Mach-O arm64. A temporary one-source edit and restore each rebuilt only that C object and the final link. Generated game inputs were read-only symlinks from the primary checkout and remain untracked. |
| GoW3 GPU import | **PASS, standalone** | Pinned source `b907d8a9f2324ba5f514fbfe7de803e027c6ca26`, shadPS4 base `b4e7ae776071ceb1dd35004577a8719481e1d258`, full native arm64 CMake build, exports, and ABI smokes verified. GoW3 alias, depth/stencil, and blend CPU regression tests pass. |
| KosmicKrisp Vulkan | **PASS, bounded** | M2 feature/workload probe passes the tested source-derived requirements. Latest standalone smoke also resolves candidate GNM NIDs, submits an 8-byte PM4 NOP, signals submit-done, and waits for the candidate queue to drain. This is not a GuestContext import or game workload. |
| MoltenVK | **PARTIAL / incompatible for renderer** | Diagnostic workloads work, but strict device creation fails on missing `robustBufferAccess2` and `nullDescriptor`. |
| Runtime guest-memory adapter | **PARTIAL** | Synthetic tests cover bounds, allocated/free VM extents, sparse holes, atomics, writeback, map/unmap/invalidation, runtime callbacks, host page protection, and 16 KiB M2 page granularity. The game does not install this API. |
| GNM dispatch / VideoOut | **NOT INTEGRATED** | `ps4_app` still dispatches GNM to the custom Metal path; candidate direct HLE pointers have no `GuestContext*` ABI adapter. Candidate and primary VideoOut/equeue ownership also remains unresolved. |

## Gate summary

| Gate | Status | Next evidence needed |
|---|---|---|
| M0 protected baseline and deterministic ARM64 build | **PASS** | Candidate clean and incremental builds plus two-generation graph test pass. |
| M1 automated splash/transition capture | **PARTIAL** | Both opening images and the first sampled bad frame are saved; exact content recognition and a PM4 rolling transition trace are not automated. |
| M2 standalone pinned GoW3 GPU | **PASS, limited lifecycle** | Native library, required GoW3 regressions, ABI/init, and candidate raw-HLE PM4 NOP plus queue-drain smoke pass. Teardown and game GuestContext imports are not tested. |
| M3 Vulkan feature/workload probe | **PASS, bounded** | KosmicKrisp passes the source-derived tested workload; guest depth semantics and full game rendering remain open. |
| M4 real guest submissions through GoW3 GPU | **NOT TESTED** | Standalone raw HLE submission works for a synthetic NOP; build the typed GuestContext import adapter and route a real game submission through it. |
| M5 guest memory, faults, EOP/events | **PARTIAL** | Synthetic adapter passes; install it in the process and verify a live protected-page fault, completion label, event, and waiter. |
| M6 shader/texture validation | **NOT TESTED** | Capture actual guest shader and descriptors through the candidate backend and validate their contents. |
| M7 deterministic replay | **NOT TESTED** | No complete guest submission capture exists. |
| M8–M9 geometry, textures, multipass | **NOT TESTED** | Require a replayable guest workload and attachment evidence. |
| M10 recognizable post-splash image | **FAIL** | Latest production capture is white/half-black after the Santa Monica splash. |
| M11–M14 audio/video, menu/gameplay, sustained performance | **NOT TESTED** | Depend on correct guest rendering and real input/presentation integration. |

Overall: **PARTIAL**. Standalone Vulkan/device success and operation counters do not pass M4 or establish game graphics correctness.
