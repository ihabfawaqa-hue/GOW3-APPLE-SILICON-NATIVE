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

## Correction: the FlushBarriers skip is not worth doing

The first draft proposed skipping `EndRendering()` in `FlushBarriers`. The `GOW3_PERF_DIAG`
"resumed" column counts pass ends after which the same pass began again (a true split). Summed
over the whole 250 s gameplay run:

| Site | Pass ends | Same pass resumed |
|---|---|---|
| `vk_runtime.cpp:879` FlushBarriers | 102,226 | 509 (0.5%) |
| `vk_runtime.cpp:265` CopyAliasImage | 37,581 | 0 |
| `vk_scheduler.cpp:80` state change | 36,022 | 0 |
| `vk_object_motion.cpp:126` | 22,015 | 0 |
| `texture_cache.cpp:1290` | 1,976 | 49 (2.5%) |
| `vk_scheduler.cpp:638` submit | 10,057 | 14 |

Almost every pass end is followed by a different pass, so removing the end would not merge
anything. The skip is also unsound: with dynamic rendering a `vkCmdPipelineBarrier2` is not allowed
inside a render instance, and the barrier cannot be hoisted into already-recorded commands.
Not implemented.

## What is left

The ~111 render + ~169 compute/blit Metal encoders per frame (SPARTAN_PROFILE_20261024.md) are
created inside KosmicKrisp, one per copy, dispatch and pass change, far above the ~14 Vulkan pass
ends per frame. Reducing them means a KosmicKrisp change (encoder reuse across compatible copy and
compute commands), or fewer copies and dispatches from the game's own command stream. Measure with
`GOW3_METAL_PASS_LABELS=1` under xctrace before choosing.
