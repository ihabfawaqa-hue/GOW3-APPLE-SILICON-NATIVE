# GoW3 GPU upstream delta and first Metal attribution

Updated: 2026-10-24. Upstream: https://github.com/Sohebdsa/gow3_pc, imported at `b907d8a` (still the head of upstream `gow3`).

## Upstream fixes

| Upstream commit | Change | Status here |
|---|---|---|
| `036d5d7` | Image aliasing / blown-out exposure | PRESENT (ancestor of import); `aliasing-test` PASS |
| `4676e1c` | Effective depth/stencil state | PRESENT; `depth-stencil-state-test` PASS |
| `9c88add` | Scaled MIN/MAX blend | PRESENT; `blend-rewrite-test` PASS |
| `cc3563a` | CPU reads/writes without waiting for GPU readbacks | PRESENT (ancestor) |
| `18ad37b` | No waits for ignored predication | PRESENT (ancestor) |
| `a101edb` (#331) | Backing-write invalidation for streamed textures | PORTED in `1f62958` (not in any fetched branch) |
| `10a7864` | Multisampled depth read by shader (RDNA2 black lines) | NEEDS_TEST |
| `6b51083`, `55eaa5d` | Optional `GOW3_NO_MSAA` | NEEDS_TEST (performance lever; changes image, opt-in only) |
| `dd25c58`, `55e6603`, `268a2fd`, `51861f3`, `60d03f3` | Capture census/dumps, debug barriers, wave-width logging | MISSING_NOT_APPLICABLE (diagnostics; local equivalents exist) |

## First Metal System Trace attribution (gameplay, attach mode, `mst1.trace`)

- 88 frames in 12.2 s (7.2 FPS, about 139 ms/frame).
- Game GPU busy (union of channels) 60%: about 83 ms/frame, of which fragment about 72 ms, vertex about 9 ms, compute about 2 ms.
- Game GPU inactive 40% (about 56 ms/frame), almost all in gaps over 1 ms (about 2.5 per frame, up to 96 ms). This alone does not establish CPU/GPU serialization; non-game GPU work overlaps some gaps.
- The initial estimate was about 2,000 Metal encoders per second. The subsequent ID census finds about 111 render and 169 compute/blit encoders per profiler-assigned frame, rather than 200 render encoders; see `SPARTAN_PROFILE_20261024.md` for methodology and limits.
- WindowServer used 26% of the GPU over the same span.
- `Scheduler::BeginRendering` uses `storeOp=Store` for every attachment and `loadOp=Load` unless cleared, including 2x MSAA targets.

## Per-pass labels

`GOW3_METAL_PASS_LABELS=1` names the Vulkan command buffer before each render pass; KosmicKrisp copies the name onto the Metal command buffer and encoders. Labels appear only when the game is launched under `xctrace record --launch` (not with `--attach`), and the 8-10 s `--window` ring buffer kept only a few app-side encoder rows, so a per-pass ranking is not done yet. Next: launch under xctrace without `--window` for a short gameplay span (start from a save or a shorter scripted path), or use an Xcode GPU frame capture.

Follow-up: the separate `metal-object-label` table recovers more timestamped labels from
the existing ring captures, but still only 13.8% of game busy time in `mst4`. A full-startup
launch trace generated an 18.9 GB temporary spool before completion. Do not repeat this
experiment without writable storage and a guard on the spool and actual free space.
