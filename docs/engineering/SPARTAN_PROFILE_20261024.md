# Operation Spartan: verified handoff and profiling audit

Date: 2026-10-24. This is a diagnostic milestone, not an accepted performance improvement.

## Verified state

- Original branch `luna/gow3-runtime-integration` was clean at `d352605`.
- Optimization branch `sol/spartan-profile-attribution` starts at that exact HEAD.
- `1f62958`, `7004de8`, and `d352605` exist with the expected changes. The untouched
  `gow3-gameplay-baseline-20261024` tag resolves to `32471fc`.
- `ninja -j4 ps4_app` and the GPU CMake build pass; app is Mach-O arm64.
- `aliasing-test`, `blend-rewrite-test`, and `depth-stencil-state-test` pass.
- Installed runtime: `20261024-172015-7004de8-dirty`; Mesa
  `09a9fae26d5df276a23b3f66050cd0d1d227800a`, Vulkan API 1.4.363.
  The manifest's pre-commit dirty revision is not an unknown binary: the current source
  build and installed GPU dylib both have SHA-256
  `f076a115a8a678e22af14c9631d0cc70bbd489496e7c687fb1f4fd27d89c229b`.
- `lsof` on the bounded live baseline confirmed both dylibs under persistent runtime storage.
- The 1 TB volume is NTFS, mounted read-only. A write probe failed with EROFS despite
  695 GiB free. No remount, driver installation, asset modification or push was attempted.

## Recovered GPU accounting

Source: Claude's existing `mst1.trace` and `mst1_gpu.xml`, in the local scratchpad
`/private/tmp/claude-501/-Users-ihabfawaqa/f4bde0d9-acdd-4e7d-891a-a3e9cc509fe8/scratchpad/`.
The new analyzer uses active interval unions and filters ownership by process. It does
not add vertex and fragment intervals where they overlap.

| Quantity | Whole window | Divided by 88 profiler-assigned frames |
|---|---:|---:|
| Window from first to last game GPU interval | 12,219.765 ms | 138.861 ms |
| Game GPU active union | 7,320.490 ms | 83.187 ms |
| Game GPU inactive | 4,899.275 ms | 55.674 ms |
| Non-game GPU work overlapping those gaps | 857.445 ms | 9.744 ms |
| Gaps with no attributed GPU work | 4,041.830 ms | 45.930 ms |

Non-game includes unknown ownership; it does not mean that all work was WindowServer.
Absence of an attributed interval is not proof that the physical device was powered idle.
The 88 frame assignments were recovered separately from
`metal-command-buffer-frame-assignment`; their `has-present` fields are all zero on this
Metal 4 path. Therefore 7.2 assigned frames/s and the averages above are retained as
profiler accounting, not independently verified display FPS or presentation percentiles.

Stage unions: fragment 6,366.091 ms, vertex 785.168 ms, compute 174.173 ms.
These are non-additive. Fragment work dominates recorded active time.

An ID census finds 9,808 distinct IDs in vertex/fragment channels and 14,861 in compute,
or approximately 111 render and 169 compute/blit encoders per assigned frame. Two IDs
occur across these categories; all-type union is 24,668. This contradicts an interpretation
of the handoff's rough estimate as 200 *render* encoders per frame.

## Label correlation and its limits

The original code names the Vulkan command buffer in `Scheduler::BeginRendering` after
ending the prior rendering scope. In Mesa, `kk_cmd_buffer_set_label` updates the active
Metal objects, and `kk_encoder_update_debug` copies the name into each new encoder.
`cs_start_render` and `cs_get_compute` both call that helper. Consequently, a compute
encoder can inherit a prior render label: labels are attribution hints, not proof of which
guest operation caused compute work.

The direct join is `(process, cmdbuffer-id, encoder-id)` from
`metal-application-encoders-list` to `metal-gpu-intervals`. The supplementary join is
`(pid, object-id)` from `metal-object-label` to the GPU encoder ID, choosing the most
recent label event no later than that GPU interval. It never guesses using nearest times
across unrelated objects or processes.

| Existing capture | Direct encoder records | Recovery including label events | Busy-time label coverage |
|---|---:|---:|---:|
| mst3 | 40 | 57 encoders with target descriptions | 3.57% |
| mst4 | 2 | 92 encoders with target descriptions | 13.81% |

The following is a ranking of **only the labeled subset of mst4**, an approximately
1,119.745 ms retained window. Values are observed lower bounds, cannot be extrapolated
to the whole frame, may overlap, and include inherited labels. The scene is not verified
as equivalent to mst1. This is not a complete top-ten gameplay ranking.

| Rank within labeled subset | Target family | Observed busy union (ms) |
|---:|---|---:|
| 1 | 1920×1080, 2× MSAA RGBA8 sRGB + D32S8 | 24.519 |
| 2 | 960×540, RGBA8 UNORM | 13.899 |
| 3 | 1920×1080, RGBA8 sRGB | 13.352 |
| 4 | 480×270, RGBA8 sRGB | 5.484 |
| 5 | 960×540, RGBA8 sRGB | 3.327 |
| 6 | 960×540, RGBA8 UNORM + D32S8 | 1.740 |
| 7 | 2048² depth D32S8 | 1.357 |
| 8 | 1920×1080, RGBA8 sRGB + RGBA8 UNORM + D32S8 | 1.135 |
| 9 | 120×67, RGBA8 sRGB | 1.061 |
| 10 | 240×135, RGBA8 sRGB | 1.011 |

The installed profiler exposes the necessary IDs; the obstacle is retained metadata and
bounded storage, not a demonstrated inability of its API to correlate encoders. A full
launch capture reached a 77-pass guest frame, but a CPU sampler timeout in the wrapper
ended tracing before save. That attempt left an 18,851,557,376-byte temporary ktrace.
The retry failed for lack of disk space. Both apps stopped; only the two temporary
ktrace files from these failed attempts were removed, restoring approximately 18 GiB.
Original traces and baseline evidence remain intact.

`record_metal.py` now guards the temporary spool and actual free space, not merely the
destination directory. Long startup traces must not be repeated under the present storage
constraints. The defensible current alternative is short attach GPU accounting, the
partial ID-based label join above, bounded guest-pass census and native wait statistics.
Full per-pass attribution remains open; a short deterministic replay/checkpoint or a
writable larger capture volume is needed before another full-launch attempt.

## CPU and queue evidence

The three-second native sample `spartan/baseline-cpu-sample.txt` contains 672 samples per
thread. The GPU command processor has 299 samples in its PM4 wait loop through
`MemoryManager::AtomicLoad`, including callback-table mutex lock/unlock work. The guest
submission thread has 556 samples waiting in `sceGnmSubmitCommandBuffersForWorkload`;
the Vulkan recorder has 542 samples waiting on its input condition variable. This is a
separate window, so these sample proportions must not be assigned to mst1's 55.674 ms.

The existing mst1 CPU table was also inspected in the exact GPU time window: running
command-processor and draw-preparation samples occur during game GPU gaps. Its template
records running threads only, without waiting-thread sampling; it cannot account for all
CPU blocking states. No single CPU stall has been shown to explain every gap.

Bounded native run `run_20261024_150022_961374` enabled `GOW3_FRAME_STATS=1` and
`GOW3_PERF_DIAG=1`. In one 8.5 FPS window it recorded 3,974.809 ms of semaphore waiting
over 43 calls at `vk_presenter.cpp:556`, plus 288.072 ms over two buffer-cache waits.
The presenter site implements the existing one-frame-ahead queue limit. Later demanding
windows report only 0–5.2% in GPU tick waits, 0.6–1.1% recorder blocking and zero shader
compiles. The bottleneck mix changes through the opening sequence. Counter percentages
are CPU-side wall-time instrumentation, not measured GPU busy percentages.

## Encoder splitting mechanism

Mesa source at the verified clean revision shows:

- `kk_CmdPipelineBarrier2`: every barrier while a multisampled render encoder is active
  applies attachment stores, ends the encoder, and starts a new one. Single-sample passes
  also split when `kk_barrier_requires_encoder_split` detects a write-to-texture-read or
  relevant depth/storage-to-input-attachment dependency.
- `cs_get_compute`: compute work inside rendering stores attachments, ends rendering,
  and marks rendering for restart on the next draw.
- `kk_flush_gfx_state`: changes in sample locations or color attachment mapping can
  force a restart.
- The render descriptor forces attachment loads for restarted encoders.

These are source-verified causes, not measured counts or GPU costs of each cause. No
barrier, load/store, aliasing, or MSAA semantics were removed.

## Resolution experiment gate

`GOW3_RENDER_RES` is the compatibility setting for an upstream startup-patched guest;
the native launcher does not apply that upstream patch. Changing it alone is not proof
that the guest's targets changed. The live host proxy path in
`SceneTargets::Eligible` explicitly rejects images or backing storage with samples != 1.
It therefore cannot establish a four-resolution comparison of the current 2× MSAA scene.

The 1080p native scene and 1710×961 presenter output are distinct sizes. Neither resizing
the output window nor scaling only the eligible single-sample proxies is an acceptable
substitute for the requested internal-resolution matrix. The lower three matrix rows,
their p95/p99 and per-resolution GPU costs remain NOT MEASURED. No diagnostic patch was
added that would silently substitute incomplete scene scaling or remove MSAA.

## Correctness and acceptance

The 245-second native run captured the opening combat tutorial at 230 seconds, with
Kratos, enemies, foliage, lighting and HUD. Last four demanding five-second windows were
7.1, 6.6, 6.3 and 6.5 FPS; median guest flip intervals 133.34–150.04 ms and p99
166.69–333.20 ms. These are changing scene windows, not an optimization comparison.
The logger does not report p95. Latest three windows report zero shader/pipeline compiles.

This verifies early gameplay remained functional. It does not verify the previously
reported late streaming-corruption scene, audio timing, or all texture-coherency cases.
The backing-write fix and prior alias/depth/blend/readback/predication fixes are unchanged.
No accepted performance patch, production setting change or FPS gain is claimed.

### Existing queue-depth A/B experiment

A second 245-second run, `run_20261024_150455_176445`, changed only the existing
`GOW3_FRAMES_AHEAD=2` setting relative to the native diagnostic baseline. The last four
reported windows were 6.8, 6.4, 6.4 and 6.2 FPS, versus 7.1, 6.6, 6.3 and 6.5 FPS with
the default. Screenshots show the same opening combat area but different camera/enemy
states and draw counts; the two runs are not frame-identical. The result establishes no
repeatable gain and does not justify a default change. The environment override ended
with its process; the default remains one frame ahead.

The new guarded recorder successfully saved and exported `queue-two-attach.trace`.
Its six-second request took 26.5 seconds including setup/save, observed 330,120,456 bytes
of temporary spool growth, and did not send any signal to the game. The independent
game watchdog ended the target at its deadline, leaving only 3.165 seconds between the
first and last attributed game GPU event. That clipped, changing workload is unsuitable
for a GPU utilization comparison. It validates the recording/export path, not a queue
optimization. Both game runs and the recorder have exited.

Tests for the diagnostic change: three Python regression cases cover overlapping GPU
stages, non-game work inside game gaps, XML id/ref sharing, process-safe encoder joins,
timestamped label fallback and partial coverage. All pass. The native build is unchanged
and all three existing renderer regression executables still pass. Full resolution,
late texture streaming, upstream MSAA changes, and audio/cinematic timing remain open.

Local evidence is under `~/Library/Application Support/GOW3Lab/spartan/` and
`~/Library/Application Support/GOW3Lab/runs/run_20261024_150022_961374/`. Captures, logs,
raw guest buffers and XML exports are deliberately excluded from Git.
