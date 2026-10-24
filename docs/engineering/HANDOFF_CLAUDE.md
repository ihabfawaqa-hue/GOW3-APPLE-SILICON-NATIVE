# Handoff for Claude: GOW3 native ARM64 Vulkan integration

**Date:** 2026-10-24
**Current milestone:** G3 — bounded real guest GPU work reaches the GoW3/shadPS4 Vulkan backend and candidate VideoOut captures frames.
**Overall verdict:** PARTIAL. No recognizable God of War scene, menu, or gameplay has been verified.

## Working tree and boundaries

- Continue in `/Users/ihabfawaqa/Downloads/gow3-arm64-runtime-int` on branch `luna/gow3-runtime-integration`.
- Production checkout `/Users/ihabfawaqa/Downloads/gow3-arm64` on `luna/operation-spartan` remains unchanged. Keep the opt-in candidate isolated until it passes broader visual and synchronization checks.
- Game assets are mounted at `/Volumes/1TB/gow3_cusa01623_extracted`. Do not modify, publish, or upload them.
- The runtime and candidate dylib both build as native `arm64`; Rosetta is not a runtime dependency.
- `./Launch_GOW3.command` is the launcher. Default graphics stay on the existing Metal implementation. The candidate route is enabled only with `PS4_GOW3GPU_BACKEND=gow3gpu-vulkan`.
- Keep run artifacts under `~/Library/Application Support/GOW3Lab/` or local Downloads. Do not add captures, raw guest memory, game data, audio mixes, or secrets to Git.

Review `git status --short` and `git log -1` before editing. Preserve existing changes; do not reset the branch or replace the repository from an archive.

## Verified result

The candidate backend initializes KosmicKrisp on the M2, registers its GNM symbols, receives real guest PM4 submissions, and its VideoOut route opens, registers three buffers, accepts guest flips, and reaches the Vulkan presenter. Run `run_20261024_064805_480945` records indexed draws and BC1/BC4 sample descriptors. It captures a readable Santa Monica Studio logo from the guest display buffer.

Two different internal captures of that logo disagree: the guest display PNG is blue/purple and the presenter-stage readback is red/beige. The latter is taken from `frame->image` before the swapchain blit, in `Presenter::PrepareFrame`; it is not a macOS desktop screenshot. Treat this as a real discrepancy in the internal presentation path, not yet as proof of the user's on-screen colors.

The four later sampled frames are black or flat. The most recent repeated run (`run_20261024_065405_105444`) has the same bounded behavior. No God of War scene follows the logo. Do not call this G8, authentic graphics, a working menu, or gameplay.

KosmicKrisp and MoltenVK both report no sparse buffer residency. The candidate has a non-sparse resident-range fallback and passes renderer-init and bounded splash-workload runs. The game log did not report a `Dense guest buffer range` allocation, so the exact cache route for the captured live resources remains unattributed. Do not assume the fallback has correct cache migration, aliasing, vertex/index/indirect binding, or guest-write semantics yet.

The run logs report `Unimplemented IT_SET_PREDICATION` from the pinned Liverpool processor. No evidence yet ties this warning to the bad post-logo image. Optional device-extension warnings are also recorded; do not treat missing optional extension names as the blocker without a workload test.

## Evidence and exports

The local export contains screenshots, bounded logs, JSON summaries, and the primary pass traces (raw framebuffer and guest-memory blobs are excluded):

- [Export README](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/README.md)
- [Primary run summary](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/primary-run-064805/summary.md)
- [Primary run stderr](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/primary-run-064805/stderr.log)
- [Guest display logo PNG](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/primary-run-064805/frames/frame_candidate_display_display_003_1920x1080.png)
- [Presenter-stage PNG](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/primary-run-064805/frames/frame_candidate_output_output_003_1710x961.png)
- [Pass trace 3](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/primary-run-064805/gpu-captures/frame_20261024-094815_3.txt)
- [Latest repeat summary](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/latest-repeat-065405/summary.md)
- [Export checksums](/Users/ihabfawaqa/Downloads/GOW3-candidate-handoff-20261024/SHA256SUMS.txt)

Original full local runs remain at `~/Library/Application Support/GOW3Lab/runs/run_20261024_064805_480945/` and `.../run_20261024_065405_105444/`, including raw dumps excluded from the export. Those files may contain proprietary derived game data; keep them local.

## Build and run evidence

Commands verified in this worktree:

```sh
ninja -j4 ps4_app
cmake --build /tmp/gow3gpu-arm64-dense-range-build --parallel 2
python3 -m py_compile scripts/lab/run_iteration.py
file ps4_app /tmp/gow3gpu-arm64-dense-range-build/libgow3gpu.dylib
lipo -archs ps4_app
lipo -archs /tmp/gow3gpu-arm64-dense-range-build/libgow3gpu.dylib
```

Both binaries report `arm64`; Ninja reports no work to do on the app. Candidate ABI, host-runtime, and KosmicKrisp renderer-init smoke executables pass, as do the pinned image-alias, depth/stencil, and scaled MIN/MAX blend regressions (see `docs/engineering/TEST_MATRIX.md`).

A representative bounded candidate run is:

```sh
PS4_GOW3GPU_BACKEND=gow3gpu-vulkan \
VK_DRIVER_FILES=/tmp/mesa-kosmickrisp-install312/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
python3 scripts/lab/iterate.py --mode smoke --skip-build \
  --runtime-seconds 60 --screenshot-every 15 \
  --max-log-mb 32 --max-capture-mb 512
```

Only one GPU-heavy instance at a time. The harness bounds runtime, logs, screenshots, and capture bytes, and terminates only the process it starts. `--mode smoke` is currently the only implemented lab mode.

## Highest-leverage next work

1. **Resolve the color discrepancy with a controlled test.** Compare the raw `display_003_1920x1080.raw` and `output_003_1710x961.raw` bytes against the recorded `R8G8B8A8Srgb` and `B8G8R8A8Unorm` tags. Add an unmistakable color-bar/offscreen probe through the same `GetFrameViewFormat` → FSR → post-process path. Inspect `third_party/gow3gpu/gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp` and `vk_swapchain.cpp`. Separate capture byte order from actual GPU color conversion. Do not patch guest texture descriptors or shaders from the logo screenshot.
2. **Attribute the live buffer path.** Add bounded structured logs for cache mode, guest address range, VkBuffer/BDA mapping, and alias generation for the captured draw. Validate resident-range allocation/migration and guest-visible writes on those same resources; then exercise vertex/index/indirect consumers.
3. **Close the completion contract.** Trace one real guest flip label and wait through PM4 submit, Vulkan completion, host callback, label store, event delivery, and waiting-thread wake. Preserve the primary runtime's event ownership unless the exact adapter contract proves a safe transfer.
4. **Capture a resource-complete post-logo pass and build deterministic replay.** The current trace has a small number of draws and BC1/BC4 identities but not all descriptors, backing bytes, and producer dependencies. Replay must include the actual resource-producing pass before changing shaders or render state.
5. Continue until repeatable recognizable God of War imagery appears. Only then proceed to menu, controller, audio/video presentation, gameplay, and performance.

Update `docs/engineering/STATUS.md`, `BLOCKERS.md`, `DECISIONS.md`, and `TEST_MATRIX.md` with evidence. The current detailed snapshot is maintained there. Keep the custom Metal path diagnostic-only and preserve the two-opening-screen baseline.
