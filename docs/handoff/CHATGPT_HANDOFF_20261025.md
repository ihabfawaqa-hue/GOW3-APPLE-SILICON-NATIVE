# Handoff: God of War III Remastered on an M2 MacBook Air (2026-10-25)

Written for the next agent (ChatGPT) by Claude, at the end of a long performance session.
Two goals: **(1) a good frame rate**, still open, and **(2) a full-screen picture instead of a
small image with black bars around it**, now fixed (section 3).

## 1. Where things are

| What | Where |
|---|---|
| Working repository (git worktree) | `/Users/ihabfawaqa/Downloads/gow3-arm64-runtime-int`, branch `sol/spartan-profile-attribution` |
| Latest commits | see `git log -3`; local only, nothing pushed |
| Production checkout (do not touch) | `/Users/ihabfawaqa/Downloads/gow3-arm64` |
| Game assets (read-only NTFS, never modify or upload) | `/Volumes/1TB/gow3_cusa01623_extracted` (CUSA01623) |
| Launcher for the user | `./Launch_GOW3_Vulkan.command` (in the repo) |
| Installed GPU runtime (versioned, `current` symlink) | `~/Library/Application Support/GOW3Lab/runtime/` |
| Benchmark run folders (logs, frame dumps) | `~/Library/Application Support/GOW3Lab/runs/run_*` |
| Engineering log (read Parts 4 and 5 first) | `docs/engineering/STRUCTURAL_BOTTLENECK_20261024.md` |
| Benchmark helpers (copied here from a temp folder) | `docs/handoff/bench/` |

### Architecture in one paragraph

`ps4_app` is the game's x86-64 code statically recompiled to ARM64 (generated `code_*.s`,
not in git) plus a C runtime (`runtime/`: kernel, memory, PS4 system libraries). Graphics go
through `libgow3gpu.dylib` (`third_party/gow3gpu/gpu/`, a shadPS4-derived renderer) → Vulkan →
KosmicKrisp (Mesa's Vulkan-on-Metal driver) → Metal. GPU threads:
- `shadPS4:GpuCommandProcessor` parses PM4 commands and prepares draws (stage A);
- `gow3:DrawRec` records them (stage B, the "draw pipe");
- `gow3:VkRecorder` executes recorded chunks into the `VkCommandBuffer`;
- `gow3:DrawPrep*`/`gow3:DrawScan` are helpers; `gow3:Copy*` are copy threads.

### Build, install, run

```bash
# GPU library (build dir lives in /tmp; regenerate with CMake if it was wiped)
cmake --build /tmp/gow3gpu-arm64-dense-range-build -j8
# Runtime binary ps4_app (repo root)
ninja ps4_app
# Install the GPU library + KosmicKrisp into a new versioned runtime and repoint `current`
scripts/lab/install_runtime.sh
# Play
./Launch_GOW3_Vulkan.command
```

**Shader cache:** shader emitter changes need a cache-version bump
(`ShaderBinaryVersion` in `vk_pipeline_serialization.cpp`, currently 10).

### Benchmarking (read before trusting any number)

`docs/handoff/bench/windows.sh <tag> [ENV=value ...]` runs a bounded 250 s session through
`scripts/lab/iterate.py` with auto-pressed input. It prints the 5 s frame-stat windows that have
more than 1,500 draws per frame (gameplay), their mean FPS, host-copy waits and the FATAL count.
`ab_late.sh` reports only windows from 175 s on (the heavier combat).

The noise is large; the session's lessons:
- **Movie skip:** the second intro movie is sometimes skipped at about 115–120 s, otherwise it
  plays to about 172 s. Runs therefore cover different amounts of gameplay (14 vs 27 windows);
  later combat is heavier.
- **Heat:** the MacBook Air is fanless. The same build gave 36.7 FPS cool and 22–30 FPS after an
  hour of back-to-back runs. Compare only interleaved A/B runs (A, B, A, B).
- **Frame-rate steps:** vsync stays on (FIFO), so frames take 16.7, 33.3 or 50 ms. Vsync off
  (Immediate) measured worse: 19 FPS.
- **Hangs:** a hung run still ends "TERMINATED_BY_LIMIT". Check the window count:
  `grep -c "Frame stats" <run>/stdout.log` should be about 49 for 250 s.
- **Per-thread timing:** "GPU thread X us/draw" in the frame stats is always 0 on macOS. Measure
  per-thread cost with `sample_at.sh` (macOS `sample`) plus `sample_busy.py` and `symincl.py`.
- **Frame dumps:** the third argument of `bench_cfg.py` is when the frame dump fires; use 200
  for a mid-combat dump. Dumps land in `<run>/frames/`.
- **One at a time:** run only one GPU-heavy instance.

### Standing rules from the user

- No push. Commits are local, with the trailer `Co-Authored-By: ...`.
- Don't reset the branch.
- Keep captures, raw guest memory, game data and secrets out of git.
- Never remove MSAA, barriers or stores globally.
- Vulkan timestamps stay off.
- Don't quote upstream PC speedups as M2 expectations.

## 2. What was achieved

| Commit | Change | Effect |
|---|---|---|
| `67489b7` | Bisected another agent's (Argon/Gemini) runtime changes, kept the good ones, skip invisible draws | Game runs past the intro movie again |
| `18dd473` | **Fast shader math** on KosmicKrisp (no Metal `precise::` math; `GOW3_PRECISE_SHADER_MATH=1` restores it); `guest-main` and the critical GPU threads at `USER_INTERACTIVE` QoS; game job workers at user-initiated | **~9 → ~30 FPS** (best runs 31–38 FPS, cool) |
| `b41c83e` | **Deadlock fix** in `runtime/kernel/memory/ps4_direct_mem.c` | About 1 run in 10 froze in gameplay; none in 5 runs since |
| `b41c83e` | Per-draw cost: fetch-shader cache looked up before the 16 KB copy; shader code checked once per submission (`submission_epoch`, toggle bit 58) | GPU command thread 92% → 71% busy; its guest-memory copies 29% → 10% of its time |
| `b41c83e` | `GOW3_RECORDING_CHECK=1`: exact detector for two threads recording one command buffer | No race in gameplay (diagnostic only) |
| latest | Safe-area ratio 1.0; launcher defaults to borderless fullscreen | Picture fills the screen (was 80% inside black borders) |

### Deadlock details

`dm_alias` and the unmap path called the renderer's invalidate hook while holding
`g_direct_mutex`. Meanwhile the main thread, inside a write-fault handler, held the texture cache
mutex and waited for `g_direct_mutex`. Remap notices are now queued per thread and delivered
after `dm_unlock()`. The renderer always takes its cache locks before guest-memory locks; keep it
that way.

## 3. What is not achieved

### Goal 1: frame rate. Now about 30 FPS cool and 22–30 hot in heavy combat; 60 is far off

Latest data point: after the machine cooled for about 2.5 hours, the full-screen run gave 38–45
FPS across the 170–215 s combat windows (1,250–2,700 draws per frame). Hot, back-to-back runs give
22–30 FPS in the same scene, so heat alone moves the result by roughly 1.5×.

Measured, and why nothing more came of it:
- **Moving guest buffer copies off the Vulkan recorder** (`GOW3_DISABLE_OPTIMIZATIONS=524288`,
  the copy pool): it halves the command processor's blocking in heavy scenes (host copies
  13–15% → 7%, recorder 6–9% → 2%). FPS doesn't change (25.9 vs 26.5), so the default is
  unchanged.
- **Cutting per-draw cost:** done (above). FPS doesn't change either, because the limit moved.

**The current limit is the CPU, specifically the game's main thread.** It is now the busiest
thread at 93%, and 37% of its samples are one recompiled guest function, `fn_0x1a5d10`, called
from `fn_0x1b00c0`. The x86 is a bare spin with no `pause`: `loop: cmp dword [rdi],0; jne loop`.
The main thread is waiting for its job workers (`WorkerThread0-4`) while burning a performance
core. The M2 has 4 performance and 4 efficiency cores. Busy threads in one sample:

| Thread | Busy |
|---|---|
| guest-main | 93% |
| brbDriver (game audio) | 82% |
| VkRecorder | 80% |
| GpuCommandProcessor | 71% |
| DrawRec | 59% |
| Five job workers | ~45% each |
| DrawScan, DrawPrep0–3 | 30–50% |

Suggested next steps, most promising first:

1. **Make `fn_0x1a5d10` stop burning a core.** Spin briefly, then yield or sleep, and measure.
   - The function is directly `bl`/`b`-called from `code_018/020/022/026/028/031.s` and defined
     in `code_025.s`. The generated `.s` files are not in git and come from an external
     recompiler, so changing it means a build step.
   - One way: rename the definition in `code_025.s` at build time (e.g. a `generate_build_ninja.py`
     rule) and link a hand-written ARM64 `_fn_0x1a5d10` that follows the recompiled ABI:
     - guest RDI is in `x24`, the guest memory base in `x28`, guest RSP in `x26`;
     - the function returns by popping the guest return address: `ldr x16,[x28,x26]; add x26,x26,#8`,
       then `ldr x30,[sp],#16; ret`.
   - Save every caller-saved host register before calling C (`sched_yield`, or a short
     `__ulock_wait` with timeout).
   - Patching code pages at run time is not an option: code signing on Apple Silicon.
   - Check with A/B runs that the workers really finish sooner; a sleep adds wake-up latency.
2. **Reduce other CPU work on the performance cores.** Candidates: brbDriver audio at 82% (why
   so high?), and the Vulkan recorder at 80%. A quarter of the recorder's time used to be guest
   buffer copies; re-profile.
3. **Smaller per-draw items left on the command processor:**
   - `CollectRingBindings` (~4%) copies constant buffers into the ring for every draw; it could
     reuse an identical (address, size) within a submission.
   - `Shader::Info::RefreshFlatBuf` and `ReadUdReg`/`GetSharp` (called twice per draw: once in
     `StageSpecialization`, once in `RefreshGraphicsStages`).
4. **Known but unexplained crashes** (rare, earlier this week):
   - `DynamicState::CommitWith` faulting at 0x20000 on DrawRec;
   - `mtl_barrier_after_encoder_stages` ← `UploadImage` ← `RefreshImage` on DrawRec;
   - the presenter's `kk_CmdBeginRendering` (in a diagnostic mode).

   The recording-race theory was tested and not confirmed. Run with `GOW3_RECORDING_CHECK=1` if
   one recurs.

Things that were tried and are worse:
- workers at `USER_INTERACTIVE` (starves the GPU threads, 13–21 FPS);
- vsync off;
- the `exp2(log2(x))` rewrite (Mesa folds it back into `pow`).

Metal GPU capture of this app crashes Apple's GPUToolsCapture on stop.

### Goal 2: full screen. Done (one sharpness item left)

Two causes, both fixed:

1. **The game drew itself at 80% size.** The guest's own 1920×1080 output buffer only had content
   in the centred 1536×864 box. `sceSystemServiceGetDisplaySafeAreaInfo`
   (`runtime/modules/libSceSystemService/ps4_system_service.c`) returned zeros, so the game read
   a safe-area ratio of 0.0. It now reports `ratio = 1.0f`, like real hardware and shadPS4. This
   file uses CRLF line endings; keep them.
2. **The launcher opened a window.** `Launch_GOW3_Vulkan.command` now exports `GOW3_FULLSCREEN=1`
   (borderless) by default. `GOW3_DISPLAY=windowed` opens a window instead. Code:
   `third_party/gow3gpu/gpu/shim/window.cpp` (`ApplyDisplayMode`).

Verified in a 215 s run with `GOW3_FULLSCREEN=1`: "Window: Borderless", and gameplay frame dumps
at 180 s and 190 s have content edge to edge in both the guest buffer (1920×1080) and the
presented frame.

Left as a choice for the user:
- **Thin bars above and below.** The screen is 16:10 (2560×1664) and the picture 16:9. Removing
  them needs a fill/crop or stretch option in the presenter's blit (`vk_presenter.cpp`, near "the
  game frame is blitted (letterboxed)", and `SetExpectedGameSize`).
- **Slightly soft picture.** The SDL window is not created high-DPI (no
  `SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN`), so the swapchain is 1710×961 points and
  macOS scales it to the panel. Enabling high DPI would present at 2560×1440 pixels. It's sharper
  but costs GPU time in the final passes (FSR and post-processing), so measure it.

## 4. Pending on the user's side

A play-test with `./Launch_GOW3_Vulkan.command` to report AJM (audio) errors, cutscene audio and
input lag. This was postponed until the frame rate is acceptable.
