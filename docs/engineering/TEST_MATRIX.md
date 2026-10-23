# Test matrix

Updated: 2026-10-23. Status is evidence-based. Synthetic and standalone tests are not game-rendering passes.

| Gate | Test | Status | Evidence / remaining work |
|---|---|---|---|
| M0 | Ninja output ownership and source language rules | **PASS** | `python3 tests/test_build_system_idempotence.py`: 316 unique outputs, distinct C/assembly/ObjC/ObjC++/C++/C++23 rules, one `gow3gpu_bridge.o`. |
| M0 | Two generator runs | **PASS** | `tools/generate_build_ninja.py` run twice; byte hashes match. |
| M0 | Clean isolated app build | **PASS** | `ninja -j4 ps4_app` completed 315 actions; output is Mach-O arm64. Generated local inputs came from read-only symlinks; no game assets entered Git. |
| M0 | Incremental compilation | **PASS** | A temporary edit and restore of `runtime/core/ps4_sfo.c` each rebuilt that one object and relinked only `ps4_app`; log is in the local GOW3Lab memory-adapter run folder. |
| M1 | Internal splash/transition capture | **PARTIAL** | Primary run `run_20261023_202535_550108` confirms Sony at 120 and Santa Monica through 1410, black at 1440, a stretched Kratos face texture at 1590, and white/black/white bands from 1650 through 5610. Full frames, selected screenshots, runtime logs, and bounded draw traces were copied to `/Users/ihabfawaqa/Downloads/GOW3-run-20261023-202535/`. The draw trace lacks matching texture/resource snapshots. |
| M1 | Bounded app smoke | **PASS, bounded** | `python3 scripts/lab/iterate.py --mode smoke --runtime-seconds 60 --screenshot-every 30 --max-capture-mb 512 --max-log-mb 32 --max-rss-mb 8192`; deadline termination after 60.872 seconds, no crash; 188 internal frames, 95 presents, and 128 partial GPU snapshots. |
| M2 | Pinned GoW3 GPU full native build | **PASS** | Candidate CMake `gow3gpu` shared library compiles/links for arm64. |
| M2 | GoW3 aliasing regression | **PASS** | `tests/test_aliasing.cpp` native arm64. |
| M2 | Effective depth/stencil regression | **PASS** | `tests/test_depth_stencil_state.cpp` native arm64. |
| M2 | Scaled MIN/MAX blend regression | **PASS** | `tests/test_blend_rewrite.cpp` native arm64. |
| M2 | Candidate ABI and host runtime smoke | **PASS** | Candidate exports and resolver checked; runtime callbacks cover errno, time, VFS, thread attach/detach, restart and recoverable fault. |
| M2 | Candidate Cocoa/Vulkan renderer and raw GNM API | **PASS, limited** | KosmicKrisp selects Apple M2, creates presenter and registers 327 GNM symbols. The standalone smoke resolves submit/done NIDs, routes an 8-byte PM4 NOP through the synthetic GuestContext marshaller into the real candidate HLE, and waits for full `work_retired`; return values are 0. Artifact: `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/gow3gpu_guest_context_20261023_m2_kosmickrisp_retirement/`. It does not call the primary app shim; teardown remains untested. |
| M3 | KosmicKrisp source-derived feature/workload probe | **PASS, bounded** | Instance/device, required features, presentation, storage-image compute/readback, offscreen barriers and image alias/view reinterpretation pass. D16S8/D24S8 unavailable; guest depth semantics remain open. |
| M3 | MoltenVK strict device creation | **FAIL** | Missing `robustBufferAccess2` and `nullDescriptor`. Its diagnostic workloads do not make it renderer-compatible. |
| M4 | GuestContext submit argument marshalling | **PASS, synthetic only** | Strict build with `-Wall -Wextra -Werror` and `gow3gpu-runtime-adapter-smoke` verify `rdi/rsi/rdx/rcx/r8`, guest address/size arrays, copied DCB/CCB bytes, retention through full work retirement, and unmapped-array rejection. The submit callback is synthetic in this unit smoke. |
| M4 | Candidate HLE via GuestContext marshaller | **PASS, synthetic workload** | `gow3gpu-renderer-init-smoke` on KosmicKrisp resolves the candidate submit/done NIDs, passes the synthetic mapped GuestContext through the marshaller to the real raw HLE, then waits for Liverpool `work_retired`. Initial run failed with an unknown opcode because submit-done alone did not guarantee buffer retirement; local wait-idle synchronization fix makes the repeat pass. Logs: `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/gow3gpu_guest_context_20261023_m2_kosmickrisp_retry/` (FAIL) and `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/gow3gpu_guest_context_20261023_m2_kosmickrisp_retirement/` (PASS). |
| M4 | Live GNM submission to candidate | **NOT TESTED** | Primary app still calls custom Metal; no game import reaches the candidate resolver or Liverpool. |
| M5 | Guest-memory adapter contract | **PASS, synthetic only** | Checks static/allocated/free ranges, sparse holes, reads/writeback, acquire atomics, map/unmap/invalidate callbacks, host page protection and M2 16 KiB tracker granularity. Not installed in a game process. |
| M5 | Live GPU page fault, EOP, event and guest wakeup | **NOT TESTED** | No candidate command has executed in the app. |
| M6 | Real shader and texture validation | **NOT TESTED** | No candidate guest shader/resource capture. |
| M7 | Deterministic replay | **NOT TESTED** | No complete PM4/state/memory capture. |
| M8–M9 | Geometry, texture sampling, multipass composition | **NOT TESTED** | Need a replayable candidate workload. |
| M10 | Recognizable post-splash image | **FAIL** | Latest production screenshot sequence becomes black, then a severely stretched face texture and persistent white/black/white bands; no correct game scene is verified. |
| M11–M14 | Audio/video, menu/gameplay, sustained performance | **NOT TESTED** | Follow graphics and presentation integration. |

Latest game evidence: `/Users/ihabfawaqa/Downloads/GOW3-run-20261023-202535/`. Candidate synthetic/renderer tests: `~/Library/Application Support/GOW3Lab/runs/gow3gpu_memory_adapter_20261023_m2/` and `/tmp/gow3_gpu_runtime_smoke_strict/gow3gpu-runtime-adapter-smoke`.
