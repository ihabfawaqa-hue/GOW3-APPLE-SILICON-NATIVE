# Operation Spartan — Luna engineering status (2026-10-25, afternoon)

| Field | Value |
|---|---|
| Branch/HEAD | `sol/spartan-profile-attribution` at `b9bd222` (local; nothing pushed). Recovery tag `spartan-known-good-20261025` → `18e2565` |
| Working tree | Clean except untracked lab files |
| Runtime revision | `20261025-134913-b9bd222` (previous `20261025-114652-18e2565` kept for rollback); KosmicKrisp Mesa `09a9fae26d` |
| Build | `ninja ps4_app` and the GPU CMake build pass (arm64) |
| Baseline scene | New Game opening combat, 300 s run: median 33.3 ms (30.0 FPS), p95 50.0 ms, p99 66.7 ms, 2,734 draws/flip (`run_20261025_105537_510818`) |
| Current scene | Player's autosave loaded (Mount Olympus, Save Altar room): 38.8–39.8 FPS at ~700 draws; 35.6–36.3 FPS with `GOW3_OUTPUT_RES=2560x1440` at ~835 draws |
| p95/p99 frame time | Baseline above; loaded-save scene NOT MEASURED (frame-stat windows only) |
| Worst transition hitch | Loading the save: one synchronous 15.5 MiB texture readback, 12.2 ms with the GPU drained. Whole-load hitch NOT MEASURED |
| GPU memory before/after stage | IOAccelerator: 1.74 GB at the menu → 3.2 GB in the opening combat (flat after); 2.03–2.04 GB after loading Mount Olympus (flat for 3 minutes) |
| Vulkan/Metal submissions per frame | NOT MEASURED this session |
| Encoder count | NOT MEASURED this session |
| Guest-main / DrawRec / VkRecorder / brbDriver CPU | NOT RE-MEASURED (last sample: 93% / 59% / 80% / 82%) |
| Audio underruns / AJM errors | NOT MEASURED by Luna (Codex run: 299 underruns in 250 s, pacing change unproven) |
| Texture correctness | PARTIAL: the load-time streaming path heals (see below); the reported persistent stripes are not reproduced |
| Lighting/shadow correctness | NOT TESTED in the reported failing scene; the loaded scene renders correctly at 1080p |
| Save picker | VERIFIED for Load Game; Save Altar NOT TESTED |
| Quick-save/load | NOT STARTED |
| Full save-state prototype | NOT STARTED |
| HiDPI | NOT TESTED by Luna (Codex: 1440p + HiDPI + FSR1 crashes or shows black) |
| Tests | aliasing, blend-rewrite, depth-stencil-state, graphics-settings, guest-memory-api, scene-resolution tests pass; guest-memory contract, audio metrics, AJM reinitialize, videodec H.264 and build idempotence tests pass |
| Accepted commits | `0ef2c37`, `a4d2d5c`, `fc6ea90`, `b9bd222` |
| Regressions | None found. The `GOW3_DIAGNOSTIC_MSAA_SCALING=1` variant of scene-resolution-test crashes inside KosmicKrisp's render encoder at HEAD as well (pre-existing) |
| Next blocker | Reproducing the persistent stage-transition stripes needs a session that reaches a later transition (driving Kratos blind is slow) |

## VERIFIED

- **Player saves protected.** The lab harness never set `PS4_SAVEDATA_DIR`, so 121 earlier bounded runs (auto-pressed New Game) wrote `param.bin`/`icon0.png` into `~/Library/Application Support/PS4Recomp/saves`. Every lab run now gets its own empty save folder and refuses to start if that folder is the player's. Read-only backup: `~/Library/Application Support/GOW3Lab/save-backups/20261025_124827`. After every run the player's folder was byte-identical to it. The current `AUTOSAVE` (Mount Olympus, 0:17:03, 12:16) is the player's own: the last lab run ended at 11:43.
- **Load Game.** `sceSaveDataDialog` was a stub with wrong enum values (LIST is 1 and PROGRESS_BAR 5; SAVE is type 1, LOAD 2). It answered every dialog at once and zeroed the game's result pointers. At a Save Altar the game opened the list, got no save directory and reopened its progress bar 2,575 times without saving. Now:
  - a LIST shows the overlay picker (picture, title, detail lines, date);
  - the chosen directory and its param are written into the game's buffers;
  - progress bars run until the game closes them;
  - the game's pad is held neutral while the overlay has input.

  On a throwaway copy of the player's save: main menu > Load showed the picker, and choosing the autosave mounted it read-only and loaded Mount Olympus.
- **Load-time texture streaming.** `GOW3_TEXTURE_ORDER_DIAG=1` shows how BC textures over the 15.5 MiB RGBA32UI pool get their data:
  - the pool is published to guest memory by a synchronous readback (12 ms);
  - the backing write then marks all 8 BC1/BC3 textures inside it for re-upload;
  - none were skipped as aliased or GPU-modified;
  - no stale binds were seen.

  Deferred readbacks are off by default, so the "readback still in flight" race does not apply to the default build.

## PARTIAL

- **Scene proxy lifetime.** Proxies of deleted images were never released (code-confirmed). They are now released after GPU completion. The path was not exercised in testing (no proxied image was deleted in a single load), and memory in 1440p load runs is unchanged.
- **Persistent texture stripes.** Two code paths could still leave a BC texture stale; the diagnostic logs both:
  - a tiled GPU-written image overlapped by a new texture is never published;
  - an existing BC texture whose pool region is rewritten by the GPU without a new overlap.

  Run the launcher with `GOW3_TEXTURE_ORDER_DIAG=1` through a transition that shows stripes; the session log then names the images involved.

## NOT CHANGED, with reasons

- **AJM planar multi-frame output.** The code does write per-frame channel blocks with `NON_INTERLEAVED`. Every logged decode is mono (`ch=1`), where the layouts are identical, and the PS4 contract for multi-frame planar output is unverified.
- **Removing `scheduler.Finish()` from readbacks.** It is the ordering guarantee of the synchronous path, and the measured cost on load is one 12 ms readback.

## Next

1. Save Altar save through the picker (in-game, with the player watching).
2. Checkpoint quick save (F5/F9) built on the game's own AUTOSAVE/save directories.
3. Reproduce a stripe transition with the diagnostic on.
4. Profile the VkRecorder heap allocations, the sampler lookup and the submissions per frame.
5. The 1440p + HiDPI + FSR1 crash in `PrepareFrame`.

## Update (evening): FPS work, audio pitch, quick save, gate ghosting

Gameplay benchmarks now load a throwaway copy of the player's save (Mount Olympus) through
main menu > Load (`PS4_SAVEDATA_DIALOG_AUTO=0`). Runtime `20261025-151116-c268c9a`.

| Item | Result | State |
|---|---|---|
| KosmicKrisp upload-block pool (cap 32 → 1024, `scripts/lab/patches/kosmickrisp-cmd-bo-pool.patch`) | Metal heap creation on the Vulkan recorder 10% → 0%; recorder 84% → 74% busy; one matched pair 35.6 → 36.3 FPS | VERIFIED, installed |
| Audio pitch/warble in gameplay | Deadline pacing (0312ef0) caused underrun bursts during play (651 after load vs 2 with device pacing); default is device pacing again (`PS4_AUDIO_PACING=deadline` opt-in); no repeated (stale) grains in either mode | VERIFIED by counters; needs the player's ear |
| Upstream video arena fix (1.25 → 2.25 GiB, pattern-checked, `GOW3_VIDEO_ARENA_PATCH=0` off) | Loads and renders correctly; effect on transition stripes not yet seen | PARTIAL |
| Quick save F5 / quick load F9 | F5 copies the last checkpoint to QUICKSAVE1 (byte-identical data); the game's Load list shows it and loads it; F9 selects it in the next Load list | VERIFIED |
| Camera-cut history reset (FSR 3.1) | No false cuts while walking; real cut (the gate attack ghosting) not reached in a driven run | PARTIAL |
| Copy pool (guest buffer copies off the recorder) | Matched loaded-save windows 45.0 vs 45.2 FPS | No gain, default kept |
| Main-thread worker spin (`fn_0x1a5d10`) | On the loaded save: ~1 wait per frame, each < 1 ms, ~2% of the main thread; a native override was built and verified, then removed (no measurable benefit) | NOT CHANGED |
| sceGnmSubmitDone lock (Argon) | Main thread blocks 14.5–19 ms/frame in combat, but the lock is checked only at the next frame's first submit: the game's logic already overlaps; the wait is the GPU command pipeline (~33 ms/frame). Releasing it early would let the game free objects before their fence labels are written (the heap corruption the lock prevents) | NOT CHANGED |
| 90 FPS (Argon's engine-rate patch) | The M2 Air's display is 60 Hz and the game logic runs at 60 Hz; frames above 60 are never shown on this panel | NOT PURSUED; target 60 |

Frame rate now: 38–45 FPS in the loaded Mount Olympus scene (~800–2,100 draws), 30–36 FPS in the
New Game opening combat (~2,500–3,100 draws). The limit is the GPU command pipeline (command
processor, draw recorder and Vulkan recorder each 70–99% busy on four performance cores).

## Update (late evening): guest memory accesses without the inline crossing check

`tools/peephole_guest_code.py` drops the 16-byte-crossing check from 120,360 of 128,203 guarded
guest loads/stores: they become `add x7, x28, x16 ; ldapr/stlr` (6 instructions and a taken
branch → 2). An access that does cross faults; `runtime/core/tso_crossing.c` runs the old
crossing path for it (plain access + the same barrier) and resumes, so x86 ordering is unchanged.
Fault path verified on the host (416 cases: all access kinds, both scratch registers, xzr, x16/x17;
2.2 µs each) and in one loaded-save run (no FATAL). That run had ~200,000 crossings/s in gameplay
(36 FPS, droppier): the game writes 8-byte values at 4-byte alignment in command-buffer builders.
The 426 sites seen are in `tools/tso_crossing_sites.txt`; every access of the 143 functions holding
them keeps its check. That build is NOT yet measured. The runtime logs `[tso]` lines with the
fault rate and top sites (`GOW3_TSO_SITES=<file>` dumps all); add new hot sites to the list.
`GOW3_GUEST_TSO_CHECKS=1 python3 tools/generate_build_ninja.py ...` restores every check.
