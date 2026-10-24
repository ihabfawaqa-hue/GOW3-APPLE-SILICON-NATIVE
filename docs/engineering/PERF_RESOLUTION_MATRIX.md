# Scene-resolution sensitivity (opening combat tutorial)

Updated: 2026-10-24. Branch `sol/spartan-profile-attribution`; runtime `20261024-202314-ffd3214-dirty`;
KosmicKrisp Mesa `09a9fae26d`. Each row is one bounded 245 s run (`PS4_PAD_AUTOPRESS=4000`) measured
over its last 300 gameplay flips (draws > 800 per flip) from `GOW3_BENCHMARK_CSV`. The runs reach
different points of the same segment, so draw counts differ; these are single runs, not a
statistically tested comparison.

| Scene raster | Switches | Median flip | Mean FPS | p95 | p99 | Draws/flip | Scene draws scaled |
|---|---|---:|---:|---:|---:|---:|---:|
| 1280x720 | `GOW3_DIAGNOSTIC_MSAA_SCALING=1 GOW3_DIAGNOSTIC_SCENE_SIZE=1280x720` | 150.0 ms | 7.02 | 183.4 | 216.7 | 2,484 | 57% |
| 1920x1080 (native) | none | 150.0 ms | 6.68 | 200.0 | 233.4 | 2,582 | 0% |
| 2560x1440 | `GOW3_DIAGNOSTIC_MSAA_SCALING=1 GOW3_DIAGNOSTIC_SCENE_SIZE=2560x1440` | 150.0 ms | 6.36 | 216.6 | 250.0 | 2,091 | 58% |

Runs: `run_20261024_170317_214335`, `run_20261024_170759_270767`, and the 2560x1440 run
following `run_20261024_171237_090537`. The 720p and 2560x1440 output frames render correctly
(HUD, Kratos, enemies, foliage); 2× MSAA is preserved per sample on the scaled targets.

Conclusion: a 3.6x change in scene pixels leaves the median frame time unchanged. Frame time is
set by fixed per-frame cost (about 40% GPU-idle CPU/GPU serialization in the Metal trace,
~110 render encoders per frame from KosmicKrisp barrier/compute splits, full attachment
load/store at every pass, fixed-size shadow and post passes), not by pixel throughput.
About 42% of scene draws and the post chain stay native in the scaled runs.

`GOW3_OUTPUT_RES` does not set the scene size: the in-game output setting overwrites it every
frame. Borderless fullscreen (`GOW3_DISPLAY=2k` in `Launch_GOW3_Vulkan.command`) only takes effect
for a foreground session; the headless harness run stayed windowed (presenter 1710x961).

A guest crash was hit once after a cutscene movie ended (`run_20261024_171237_090537`):
`FATAL: Unresolved indirect jump/call to 0x6951980 (from RIP=0x1b50150)`, a call through a
pointer into heap memory right after `my_decode_thread` exited. Not yet reproduced or traced.
