# Submit coalescing (Spartan report fix #8): measurement and proposal

Measured at e20d966 + local commits, 2026-10-26, M2 Air, throwaway copy of the Mount Olympus save,
`docs/handoff/bench/bench_cfg.py` (250 s runs). Tools: `GOW3_SUBMIT_TRACE=1` (new; call stacks of
every `vkQueueSubmit`) and the existing `GOW3_PERF_DIAG=1` (pass-end sites).

## vkQueueSubmit per frame: about 3, not 58-75

42,000 submits over 13,900 frames (2,084 draws/flip). Three stacks account for 99%:

| Stack | Per frame |
|---|---|
| `Rasterizer::Flush` from `Liverpool::Process` (end of the frame's GPU work) | 1.0 |
| `Presenter::Present` (swap thread) | 1.0 |
| `Presenter::PrepareFrame` from `SubmitFlipInternal` | 1.0 |
| `Scheduler::Finish` (`BufferCache::GetDenseArena`, `ProcessDownloadImages`) | 0.03 |

The report's "58-75 submissions" matches the "N submissions" figure in the stall line, which counts
guest GNM submissions, not Vulkan. There is nothing to coalesce at the vkQueueSubmit level.

## Where the encoder splits come from

Vulkan render-pass ends per frame (`GOW3_PERF_DIAG`, ~300-frame windows, ordinary gameplay):

| Site | Per frame | What it is |
|---|---|---|
| `vk_runtime.cpp:879` `Runtime::FlushBarriers` | ~4 (up to ~31 in a transition) | any pending image/memory barrier ends the pass |
| `vk_runtime.cpp:265` `Runtime::CopyAliasImage` | ~4 | aliased image copy |
| `vk_scheduler.cpp:80` `BeginRendering` | ~3 | the render state really changed |
| `vk_object_motion.cpp:126` | ~2 | motion pass |
| `vk_scheduler.cpp:638` submit | 1 | |

That is ~14 Vulkan pass ends per frame, but SPARTAN_PROFILE_20261024.md counts ~111 render and
~169 compute/blit Metal encoders per frame. The rest of the split happens inside KosmicKrisp: one
encoder per copy, dispatch and barrier-induced pass change.

## Proposal

1. `FlushBarriers`: skip `EndRendering()` when no barrier touches an image that is a current
   attachment or an input attachment of the open pass and the barrier is not a
   framebuffer-local dependency. This is the only Vulkan-side split that scales with scene
   complexity (4 -> 31 per frame in transitions). Validate with `GOW3_PERF_DIAG` (site count
   down, "resumed" column unchanged) and a pixel comparison of frame dumps.
2. Hoist staging-to-device copies ahead of `BeginRendering` (the report's idea) only after (1), and
   only for the copies `GOW3_PERF_DIAG` shows interrupting a pass.
3. Count Metal encoders with `GOW3_METAL_PASS_LABELS=1` under xctrace before and after, since the
   Vulkan pass count does not predict the Metal one.

Not proposed: merging submits (nothing to merge), the report's "58-75 -> <12".
