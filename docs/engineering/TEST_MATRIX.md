# Test matrix

Updated: 2026-10-23. Status is evidence-based. Synthetic and standalone tests are not game-rendering passes.

| Gate | Test | Status | Evidence / remaining work |
|---|---|---|---|
| M0 | Ninja output ownership and source language rules | **PASS** | `python3 tests/test_build_system_idempotence.py`: 316 unique outputs, distinct C/assembly/ObjC/ObjC++/C++/C++23 rules, one `gow3gpu_bridge.o`. |
| M0 | Two generator runs | **PASS** | `tools/generate_build_ninja.py` run twice; byte hashes match. |
| M0 | Clean isolated app build | **PASS** | `ninja -j4 ps4_app` completed 315 actions; output is Mach-O arm64. Generated local inputs came from read-only symlinks; no game assets entered Git. |
| M0 | Incremental compilation | **PASS** | A temporary edit and restore of `runtime/core/ps4_sfo.c` each rebuilt that one object and relinked only `ps4_app`; log is in the local GOW3Lab memory-adapter run folder. |
| M1 | Internal splash/transition capture | **PARTIAL** | Latest primary run saves both opening cards. Santa Monica sample 840 → first observed white sample 900 → half-black sample 1200. Human visual check confirms the cards; image recognition remains unverified. |
| M1 | Bounded app smoke | **PASS, bounded** | `python3 scripts/lab/iterate.py --mode smoke --runtime-seconds 45 --screenshot-every 60`; deadline termination, no crash; 44 frames and partial GPU snapshot. |
| M2 | Pinned GoW3 GPU full native build | **PASS** | Candidate CMake `gow3gpu` shared library compiles/links for arm64. |
| M2 | GoW3 aliasing regression | **PASS** | `tests/test_aliasing.cpp` native arm64. |
| M2 | Effective depth/stencil regression | **PASS** | `tests/test_depth_stencil_state.cpp` native arm64. |
| M2 | Scaled MIN/MAX blend regression | **PASS** | `tests/test_blend_rewrite.cpp` native arm64. |
| M2 | Candidate ABI and host runtime smoke | **PASS** | Candidate exports and resolver checked; runtime callbacks cover errno, time, VFS, thread attach/detach, restart and recoverable fault. |
| M2 | Candidate Cocoa/Vulkan renderer and raw GNM API | **PASS, limited** | KosmicKrisp selects Apple M2, creates presenter and registers 327 GNM symbols. The standalone smoke resolves `sceGnmSubmitCommandBuffers` and `sceGnmSubmitDone` by NID, submits an 8-byte PM4 NOP, then waits on diagnostic queue-drain; both calls and drain return 0. Run artifact: `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/gow3gpu_pm4_drain_20261023_200046_m2_kosmickrisp/`. It does not call the primary GuestContext shim; teardown remains untested. |
| M3 | KosmicKrisp source-derived feature/workload probe | **PASS, bounded** | Instance/device, required features, presentation, storage-image compute/readback, offscreen barriers and image alias/view reinterpretation pass. D16S8/D24S8 unavailable; guest depth semantics remain open. |
| M3 | MoltenVK strict device creation | **FAIL** | Missing `robustBufferAccess2` and `nullDescriptor`. Its diagnostic workloads do not make it renderer-compatible. |
| M4 | Live GNM submission to candidate | **NOT TESTED** | Primary app still calls custom Metal; the direct raw-HLE NOP smoke does not exercise a GuestContext import or game submission. Typed import bridge is not implemented. |
| M5 | Guest-memory adapter contract | **PASS, synthetic only** | Checks static/allocated/free ranges, sparse holes, reads/writeback, acquire atomics, map/unmap/invalidate callbacks, host page protection and M2 16 KiB tracker granularity. Not installed in a game process. |
| M5 | Live GPU page fault, EOP, event and guest wakeup | **NOT TESTED** | No candidate command has executed in the app. |
| M6 | Real shader and texture validation | **NOT TESTED** | No candidate guest shader/resource capture. |
| M7 | Deterministic replay | **NOT TESTED** | No complete PM4/state/memory capture. |
| M8–M9 | Geometry, texture sampling, multipass composition | **NOT TESTED** | Need a replayable candidate workload. |
| M10 | Recognizable post-splash image | **FAIL** | Latest production screenshot sequence becomes white and half-black. |
| M11–M14 | Audio/video, menu/gameplay, sustained performance | **NOT TESTED** | Follow graphics and presentation integration. |

Latest game evidence: `/Users/ihabfawaqa/Downloads/GOW3-run-20261023-194648/`. Candidate synthetic/renderer tests: `~/Library/Application Support/GOW3Lab/runs/gow3gpu_memory_adapter_20261023_m2/`.
