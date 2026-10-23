# Verified blockers

Updated: 2026-10-23.

| Severity | Blocker | Evidence | Next falsifiable action |
|---|---|---|---|
| Critical | The production application still uses custom Metal for GNM and VideoOut. | Primary run `run_20261023_194648_251517` reports `native-metal`; the candidate library is absent from the game process. | Add a typed adapter for actual imported GNM symbols, initialize the candidate once, and prove a real guest submission enters its Liverpool processor. |
| Critical | Candidate HLE function pointers do not match the primary `GuestContext*` shim ABI. | `gow3gpu_resolve` returns direct HLE functions; primary shims decode registers and guest pointers. The candidate submit signature consumes host command-buffer pointer arrays. | Define and test per-import argument decoding, guest VA validation, VA-to-host translation, return values, and command-buffer size checks before dispatch. |
| Critical | VideoOut and GPU event/completion ownership is split. | Candidate registers its own GNM, VideoOut, and kernel event symbols; the primary runtime still owns guest imports and wait queues. | Specify one presenter and one event/EOP path, then test submission → GPU completion label → equeue delivery → guest wakeup. |
| Critical | The first observed post-splash frames are corrupted. | Sample 840 is the Santa Monica logo; sample 900 is all white; sample 1200 is half-black. The active production path remains Metal. | Route one representative real submission through the candidate and capture its render target, PM4, shaders, descriptors, and completion state. |
| High | Guest-memory callbacks have only synthetic coverage. | Strict allocated/free extent, sparse read, atomic, writeback, page protection and invalidation tests pass; the app has no candidate loader/install call. | Install the versioned memory/host tables in an isolated runtime and verify one live resource mapping, CPU write invalidation and handled protected-page fault. |
| High | No complete guest GPU capture/replay exists. | The smoke run saves a partial Metal draw snapshot, not full source PM4 and all guest-memory dependencies. | Add a bounded transition trace at the chosen backend's submission boundary, then replay one complete pass. |
| Medium | Candidate renderer has no coordinated shutdown API. | Init smoke exits its own process after presenter and GNM registration; `gow3gpu.h` exposes no shutdown call. | Add lifecycle teardown for presenter, SDL/main-queue timer, GPU queues, and kernel service before enabling in the game. |

No gate above G2 is claimed from draw counts, feature names, standalone initialization, or synthetic callbacks alone.
