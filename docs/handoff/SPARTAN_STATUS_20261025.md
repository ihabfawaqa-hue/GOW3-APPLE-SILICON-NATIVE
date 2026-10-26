# Operation Spartan status — 2026-10-25

## Scope and repository state

- Repository: `<repo>`
- Branch: `sol/spartan-profile-attribution`
- Starting revision: `8f375b5e3c79a7b525659f159017b6f779666902`
- Production checkout was not modified. Game assets remained read-only.
- The ARM64 `ps4_app` and the local Vulkan runtime build successfully. Runtime installs are versioned under `~/Library/Application Support/GOW3Lab/runtime/`.
- Changes are local. Nothing was pushed.

## Changes made

- **Audio pacing:** `sceAudioOutOutput` and `sceAudioOutOutputs` now use absolute monotonic deadlines for each output grain, including while CoreAudio is running. Ring occupancy steers the period by at most 1/32 of a grain. Timing and queue measurements are logged.
- **AJM stream reuse:** decoder initialization clears sample and gapless state before applying the new stream's sideband. ATRAC9 metadata and RIFF bookkeeping are reset for reused instances.
- **VideoDec:** H.264 packet `attached_data` follows frames through B-frame reordering with `AV_CODEC_FLAG_COPY_OPAQUE`; decode after a drained flush recovers from `AVERROR_EOF`; the NV12 output allocation is reused, with a direct copy for NV12 input and padded rows filled from the last image row.
- **Engine timing and display options:** the launcher supplies the actual 60 Hz engine rate by default. `GOW3_HIGH_DPI=1` opts into SDL high pixel density on macOS; `GOW3_OUTPUT_RES=2560x1440` selects the 1440p renderer target. Existing FSR1 and RCAS switches remain available.
- **Benchmark parsing:** the gameplay classifier uses draw-count changes when `scene_draws` is unavailable. Audio metrics distinguish AJM `NOT_ENOUGH_ROOM` flow control from decoder errors. The capture trigger is aligned with the 250-second run duration.

## Verification

- `ninja ps4_app`: pass; output is Mach-O ARM64.
- `cmake --build /tmp/gow3gpu-arm64-dense-range-build --parallel 8`: pass; output library is ARM64.
- `graphics-settings-test`: pass, including the 2560×1440 environment override.
- `python3 tests/test_audio_metrics.py`: pass, 7 tests.
- `scripts/lab/test_ajm_reinitialize.sh`: pass against the AJM implementation.
- `python3 tests/test_videodec_h264.py`: pass. It decodes a 12-frame H.264 stream with B-frame reordering, verifies each output PTS has its matching input `attached_data`, drains the stream, then submits another segment to the same decoder.
- Launcher and benchmark shell syntax, Python compilation, and whitespace checks passed.

## Runtime observations

### 250-second gameplay run with audio pacing

Run `run_20261025_092844_446648` ended at the requested deadline after 250.5 seconds, with no fatal exit. It produced 12 five-second gameplay frame-stat windows above 1,500 draws/frame: mean 38.7 FPS, median 40.5 FPS, range 17.7–43.0 FPS. This workload did not match the earlier baseline scene, so the numbers do not establish a controlled FPS gain. The older baseline had 11 qualifying windows at 33.2 FPS mean and 31.5 FPS median.

The audio output log showed typical output intervals near 5.33 ms, with planned waits around 4.1 ms and queue minima usually 768–1,024 frames. Across the full parsed run, occasional stalls remained: a 96.6 ms maximum interval and 46 cadence resets. The run recorded 299 underruns; an intervening pre-pacing run recorded 2,219 and the earlier baseline recorded 160, so the runs do not prove an improvement against the baseline. Audio was not assessed by listening, and cutscene synchronization was not independently measured.

The `brbDriver` sample had 553 running and 195 waiting samples (74% active). An earlier, different workload had 583 running and 12 waiting (98% active). This is consistent with reduced busy time, but the runs differ and do not validate the proposed `<3%` CPU target.

### HiDPI and 1440p checks

- With `GOW3_HIGH_DPI=1`, `GOW3_OUTPUT_RES=2560x1440`, and FSR1 disabled, a 45-second run completed and the captured opening quote screen rendered. Logs reported a 2560×1440 scene and 1280×720 half-resolution targets. The drawable was 3420×1923 on the tested display. The run did not reach gameplay.
- With 2560×1440 plus FSR1 and HiDPI, one run crashed in `Vulkan::Presenter::PrepareFrame` with signal 11. A repeat did not crash but captured a black output frame. Treat this combination as failing; use FSR1 off for the 1440p HiDPI path.
- FSR1 with 2560×1440 and HiDPI off completed and showed the opening quote screen. A separate default-resolution HiDPI + FSR1 run also showed the quote screen. These short intro runs do not establish gameplay stability or performance.

## Remaining work and verdict

- **Guest-main spin: not fixed.** Sampling still shows `fn_0x1a5d10` as a hot leaf, while `guest-main` had 615 running and 133 waiting samples. The generated guest function is not tracked as source, and there is no reproducible source transform or focused ABI/wait-semantics test in this checkout. No hand-written replacement was made. The supplied disassembly is strong source-level evidence, but it does not identify every flag writer, wait duration, or lost-wakeup constraint. No additional worker-QoS change was made because the current default is already user-initiated, and prior broad promotion to interactive reduced FPS.
- **Worker QoS is already user-initiated by default.** `runtime/kernel/threading/ps4_threading.c` applies `QOS_CLASS_USER_INITIATED` to GoW3 `WorkerThread*` threads unless overridden. The pasted claim that these workers are currently default-class does not match this checkout; raising all workers to interactive remains a known regression.
- **brbDriver: reduced activity is suggestive, not a validated fix.** The output clock is now steady; CPU attribution needs an identical-scene A/B run to establish the amount of saved CPU.
- **FPS and thermals: incomplete.** The long run supports ongoing gameplay and about 40 FPS in its sampled windows, but scene matching, cool/hot A/B, and a sustained thermal test were not completed.
- **Retina/FSR1: partial.** Optional high-density presentation and the 1440p render target work separately and together with FSR1 off. The combined 1440p + HiDPI + FSR1 path fails as described above.
- **Cutscene quality and sync: partial.** The decoder behavior now has a focused B-frame/reuse test, but gameplay logs alone do not prove movie audio/video synchronization.

**Overall: PARTIAL.** The audio cadence, AJM lifecycle, VideoDec metadata/reuse, engine-rate default, and optional display controls have implementation and targeted test coverage. The main guest spin, controlled performance attribution, sustained thermal behavior, cutscene audio, and the 1440p HiDPI + FSR1 combination remain open.
