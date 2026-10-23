# Local GOW3 test laboratory

The implemented mode is a bounded startup smoke run:

```sh
python3 scripts/lab/iterate.py --mode smoke
```

It builds with Ninja, verifies the executable is ARM64, checks the mounted asset directory, launches only through `Launch_GOW3.command`, records internal framebuffer and GPU draw snapshots, and writes a JSON verdict. Screenshots and proprietary captures go to `~/Library/Application Support/GOW3Lab/runs/` by default. Set `GOW3_LAB_RUNS` to use another local artifact directory.

Use `--build-only` to verify the build and architecture without launching the game. Runtime duration is limited to 300 seconds; log, capture, and process RSS limits are enforced. Thermal and memory-pressure samples are recorded when macOS provides them.

To stop an active run, press **Control-C** in its terminal. The runner sends SIGTERM to the process group it started, waits five seconds, then uses SIGKILL on that same group if needed. If the terminal is still open but Control-C is unavailable, send SIGINT to the printed `RUNNER_PID`. The runner will not terminate an already-running `ps4_app` it did not start.

Only `smoke` is advertised. Transition, replay, menu, and gameplay modes are not implemented yet. A captured frame or GPU counter does not prove correct game rendering; visual authenticity remains unverified until a recognized frame is compared with a reliable reference.
