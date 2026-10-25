"""Run one bounded gameplay configuration and summarize per-flip timing (GOW3_BENCHMARK_CSV)."""
import csv, os, statistics, subprocess, sys, time
from pathlib import Path

S = Path(__file__).resolve().parent
tag, secs, trigger_at = sys.argv[1], sys.argv[2], sys.argv[3]
extra = [a for a in sys.argv[4:] if "=" in a]
csv_path = S / f"bench_{tag}.csv"
if csv_path.exists():
    csv_path.unlink()
args = [sys.executable, str(S / "run_trigger.py"), secs, trigger_at, "PS4_PAD_AUTOPRESS=4000",
        "GOW3_FRAME_STATS=1", f"GOW3_BENCHMARK_CSV={csv_path}"] + extra
out = subprocess.run(args, capture_output=True, text=True).stdout
runs = Path.home() / "Library/Application Support/GOW3Lab/runs"
run = max(runs.iterdir(), key=lambda d: d.stat().st_mtime)
print(f"[{tag}] run {run.name}; " + " ".join(l for l in out.splitlines() if l.startswith("RUNTIME")))
rows = list(csv.DictReader(open(csv_path))) if csv_path.exists() else []
game = []
for prev, cur in zip(rows, rows[1:]):
    draws = int(cur["draws"]) - int(prev["draws"])
    # scene_draws is not populated by all runtime configurations. Gameplay is
    # still visible in the cumulative draw counter: intro/menu frames stay near
    # 13 draws, while gameplay is hundreds to thousands of draws per flip.
    if draws > 500:
        scene = int(cur["scene_draws"]) - int(prev["scene_draws"])
        game.append(((int(cur["time_ns"]) - int(prev["time_ns"])) / 1e6, draws, scene,
                     int(cur["reduced_draws"]) - int(prev["reduced_draws"])))
tail = game[-300:]
if not tail:
    print(f"[{tag}] no gameplay flips recorded ({len(rows)} flips)")
    sys.exit(0)
ms = sorted(t[0] for t in tail)
q = lambda p: ms[min(len(ms) - 1, int(p * len(ms)))]
draws = statistics.median(t[1] for t in tail)
scene_draws = sum(t[2] for t in tail)
scaled = (f"{100 * sum(t[3] for t in tail) / scene_draws:.0f}%"
          if scene_draws else "not reported")
print(f"[{tag}] gameplay flips {len(game)} (last {len(tail)}): median {q(0.5):.1f} ms "
      f"({1000 / q(0.5):.1f} FPS), p95 {q(0.95):.1f} ms, p99 {q(0.99):.1f} ms, "
      f"mean FPS {1000 * len(ms) / sum(ms):.2f}, draws/flip {draws:.0f}, "
      f"scaled scene draws {scaled}")
frames = sorted((run / "frames").glob("frame_candidate_output_*.png"))
if frames:
    dst = S / f"bench_{tag}.png"
    subprocess.run(["sips", "-Z", "1280", str(frames[-1]), "--out", str(dst)], capture_output=True)
    print(f"[{tag}] last output frame -> {dst.name} ({frames[-1].name})")
