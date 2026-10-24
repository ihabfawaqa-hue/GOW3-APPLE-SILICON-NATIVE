# Local GOW3 test laboratory

The implemented mode is a bounded startup smoke run:

```sh
python3 scripts/lab/iterate.py --mode smoke
```

It builds with Ninja, verifies the executable is ARM64, checks the mounted asset directory, launches only through `Launch_GOW3.command`, records internal framebuffer and GPU draw snapshots, and writes a JSON verdict. Screenshots and proprietary captures go to `~/Library/Application Support/GOW3Lab/runs/` by default. Set `GOW3_LAB_RUNS` to use another local artifact directory.

Use `--build-only` to verify the build and architecture without launching the game. Runtime duration is limited to 300 seconds; log, capture, and process RSS limits are enforced. Thermal and memory-pressure samples are recorded when macOS provides them.

To stop an active run, press **Control-C** in its terminal. The runner sends SIGTERM to the process group it started, waits five seconds, then uses SIGKILL on that same group if needed. If the terminal is still open but Control-C is unavailable, send SIGINT to the printed `RUNNER_PID`. The runner will not terminate an already-running `ps4_app` it did not start.

Only `smoke` is advertised. Transition, replay, menu, and gameplay modes are not implemented yet. A captured frame or GPU counter does not prove correct game rendering; visual authenticity remains unverified until a recognized frame is compared with a reliable reference.

## Metal profiling

`record_metal.py --pid <running-game-pid> --output <local-path>.trace` records a short
attach capture, leaves the game running, and guards actual free disk space and growth of
Instruments' temporary `instruments*.ktrace` files. The temporary spool can be much larger
than the final trace bundle. It defaults to 8 seconds, a 1 GiB spool budget and a 6 GiB disk
reserve. Check the `.recording.json` sidecar for truncation, then confirm the trace can be
exported. Do not capture an entire startup sequence without a measured spool budget.

Export the `metal-gpu-intervals`, `metal-application-encoders-list`, and
`metal-object-label` tables with `xctrace export`. Analyze them with:

```sh
python3 scripts/lab/analyze_metal_trace.py gpu.xml --encoders encoders.xml --object-labels labels.xml
python3 tests/test_metal_trace_analysis.py
```

The analyzer joins encoder IDs within a process, uses timestamped object labels as a
fallback, and computes interval unions instead of adding overlapping stages. It reports
label coverage, render versus compute/blit encoder counts, and non-game GPU activity
during game gaps. A partial label ranking cannot establish the most expensive families
across the whole workload. Instruments frame assignments and guest flip intervals are
also different from actual display presentation times. Keep all exported tables local.

## Per-pass GPU time (KosmicKrisp commit feedback)

Instruments recorded encoder labels for only ~5% of GPU busy time. Exact per-pass GPU time
comes from the driver instead. Apply `patches/kosmickrisp-diagnostics.patch` to the Mesa tree
(it adds `MESA_KK_GPU_TIMES=<file>`: one line per Metal commit with GPU start/end and the command
buffer's name), rebuild and install the runtime, then run with:

```sh
GOW3_METAL_PASS_LABELS=1 GOW3_DIAGNOSTIC_PASS_SUBMIT=1 MESA_KK_GPU_TIMES=kk_times.csv \
GOW3_BENCHMARK_CSV=bench.csv
python3 scripts/lab/pass_gpu_times.py kk_times.csv bench.csv 300
```

`GOW3_DIAGNOSTIC_PASS_SUBMIT=1` submits whenever the render targets change (before the draw
binds anything), so each commit holds one pass plus the work up to the next. It multiplies
submissions; use it only for attribution. `GOW3_PERF_DIAG=1` also prints `Pass end:` lines:
where render passes were ended, and how often the same pass resumed right after (a split).
