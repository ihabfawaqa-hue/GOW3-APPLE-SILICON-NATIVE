# Metal encoders per frame, by KosmicKrisp command (2026-10-26)

Counted inside the driver, not with xctrace: `scripts/lab/patches/kosmickrisp-encoder-trace.patch`
adds `KK_ENCODER_TRACE=1`, which counts every compute-encoder request per call site, how many ended
an open render encoder, how many opened a new compute encoder, and every render encoder started.
Run: `docs/handoff/bench/bench_cfg.py` 250 s on the Mount Olympus save copy, 13,543 flips
(menu and loading included; gameplay windows were ~727 draws/flip). Driver built from
/tmp/mesa-kosmickrisp-src with the 1024-BO pool patch.

## Totals

| | Total | Per flip |
|---|---|---|
| Render encoders started (`cs_start_render`) | 382,849 | 28.3 |
| Compute-encoder requests | 480,000 | 35.4 |
| New compute encoders opened | ~129,000 | 9.5 |
| **Metal encoders, render + compute** | | **~38** |

The report's "200-350 per frame" and the earlier census's 111 render + 169 compute/blit are not
what this build creates per game frame. The census divided by profiler-assigned frames (7.2/s under
the trace), not flips. At 38 encoders/frame there is no large encoder-count problem to fix.

## By call site (compute requests; a forced split ends an open render encoder)

| Site | Calls | Per flip | Forced render end | New compute |
|---|---|---|---|---|
| `kk_heap` first use per command buffer (`kk_cmd_draw.c:1064`) | 68,539 | 5.1 | 68,539 | 68,539 |
| tessellation / unroll precompiled kernels (`precomp` 4, 7, 12; `kk_cmd_draw.c:1979`) | 342,690 | 25 | 0 (compute already open) | 0 |
| `copy_image` (`kk_cmd_copy.c:300`) | 50,912 | 3.8 | 0 | 50,908 |
| `vkCmdCopyBufferToImage` (`kk_cmd_copy.c:94`) | 13,763 | 1.0 | 0 | 7,721 |
| dispatch (`kk_cmd_dispatch.c:23, 83`) | 3,978 | 0.3 | 0 | 1,911 |
| other copies | 118 | | 0 | 63 |

- The 5.1 forced splits per flip come from the first tessellation/unroll use in each Vulkan command
  buffer. The code comment says those paths already need a compute break, so this is not removable
  without removing tessellation/unroll from those draws.
- `copy_image` and `CopyBufferToImage` open a new compute encoder almost every call because a render
  encoder started in between. They are the main source of render/compute alternation (each pair is a
  tile store/load). Hoisting these copies ahead of the render work in the scheduler is the only
  change this data supports, worth at most ~5 encoder pairs per frame.
- Which draws use tessellation/unroll (rect lists, quad lists, primitive restart?) is not yet
  identified; that decides whether the 5.1/frame can shrink.

## Conclusion for report fix #8

Neither submits (~3/frame) nor encoders (~38/frame) is large. The expected "+4-7 FPS" from cutting
~65 submits to <12 and ~250 encoders to <45 does not apply to this build.

## Follow-up: which draws force the splits

`KK_ENCODER_TRACE=1` also counts why each draw would unroll (`kk_unroll_trace`, same patch).
Over 2,300,000 draws (13,668 flips; 1.76 M indexed, 0 indirect):

| Topology | Draws | Unrolled |
|---|---|---|
| prim 4 (triangle list) | 2,155,640 | 0 |
| prim 5 (triangle strip) | 70,364 | 0 |
| prim 14 (patch list) | 73,996 | tessellated, 5.4 per flip |

Triangle fans, 8-bit indices, restart, robustness and flat-shading unrolls: all zero. **Tessellation
is the only thing that forces compute.** That is the 5.1 forced render-encoder splits per flip, and
the ~25 libkk kernel dispatches per flip (heap clear, VS/TCS launch, tessellator).

## Upper bound on a fix

`KK_SKIP_TESS=1` (diagnostic, wrong picture) drops tessellated draws. Matched 726-draw scene,
250 s runs, 300 gameplay flips each, machine NOT idle (load average 4-10, so noisy; medians are
quantized at 33.3 ms):

| Run | Mean FPS |
|---|---|
| control | 30.0 |
| skip tess | 33.4, 32.5 |

About +2.5 to +3.4 FPS is the ceiling for removing all tessellation work and its splits together;
the split cost alone is smaller. A first control run landed in a different scene (1,625 draws,
13.8 FPS) and is not comparable.

## Can the splits shrink?

Not cheaply. Each tessellated draw needs compute (VS, TCS, tessellator) before its render work, so
a split per draw is inherent to KosmicKrisp's emulation. The only structural fix is batching:
run the compute stage of several consecutive patch draws in one compute encoder, then restart
the render pass once. That is valid only if the batched draws' compute inputs do not depend on
rendering done in between, which the driver cannot assume. Worth about 2-3 FPS at most, at
the price of a driver reordering change. Not pursued. Repeat the A/B on an idle machine before
revisiting.
