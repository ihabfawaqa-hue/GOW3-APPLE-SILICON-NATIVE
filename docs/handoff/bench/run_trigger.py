"""Run a bounded candidate session and fire pass-trace/frame-dump triggers at given times."""
import os, subprocess, sys, time
from pathlib import Path
repo = "/Users/ihabfawaqa/Downloads/gow3-arm64-runtime-int"
runs = Path.home() / "Library/Application Support/GOW3Lab/runs"
secs = sys.argv[1]; times = [float(t) for t in sys.argv[2].split(",")]
extra_env = dict(a.split("=", 1) for a in sys.argv[3:] if "=" in a)
env = dict(os.environ,
    PS4_GOW3GPU_BACKEND="gow3gpu-vulkan",
    PS4_GOW3GPU_LIBRARY=os.path.expanduser("~/Library/Application Support/GOW3Lab/runtime/current/libgow3gpu.dylib"),
    VK_DRIVER_FILES=os.path.expanduser("~/Library/Application Support/GOW3Lab/runtime/current/kosmickrisp_icd.json"),
    )
env.update(extra_env)  # overrides (a driver or library under test) win over the defaults
before = set(runs.iterdir())
p = subprocess.Popen([sys.executable, "scripts/lab/iterate.py", "--mode", "smoke", "--skip-build",
    "--runtime-seconds", secs, "--screenshot-every", "15", "--max-log-mb", "32", "--max-capture-mb", "768",
    "--frame-dump-every", "1000000", "--frame-dump-max", str(len(times) + 1),
    "--pass-capture-every", "1000000", "--pass-capture-max", str(len(times) + 1)],
    cwd=repo, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
start = time.time(); run_dir = None
for t in times:
    while time.time() - start < t:
        time.sleep(0.5)
    if run_dir is None:
        new = [d for d in runs.iterdir() if d not in before]
        run_dir = max(new, key=lambda d: d.stat().st_mtime) if new else None
    if run_dir:
        (run_dir / "candidate-pass-trace.trigger").touch()
        (run_dir / "candidate-frame-dump.trigger").touch()
        print(f"triggered at {t}s in {run_dir.name}")
print(p.communicate()[0][-900:])
