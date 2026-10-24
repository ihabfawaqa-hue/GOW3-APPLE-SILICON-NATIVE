"""Per-pass GPU time from MESA_KK_GPU_TIMES, over the gameplay flips of a GOW3_BENCHMARK_CSV.
Usage: pass_gpu_times.py kk_times.csv bench.csv [last_n_flips]"""
import csv, re, sys, collections
kk = []
for line in open(sys.argv[1]):
    a, b, label = line.rstrip("\n").split(",", 2)
    kk.append((float(a), float(b), label))
rows = list(csv.DictReader(open(sys.argv[2])))
n = int(sys.argv[3]) if len(sys.argv) > 3 else 300
# Align clocks: the last GPU commit ends just before the last flip is recorded.
offset = int(rows[-1]["time_ns"]) / 1e9 - max(e for _, e, _ in kk)
flips = [(int(r["time_ns"]) / 1e9 - offset, int(r["draws"])) for r in rows]
game = [(flips[i - 1][0], flips[i][0]) for i in range(1, len(flips)) if flips[i][1] - flips[i - 1][1] > 1500]
game = game[-n:]
t0, t1 = game[0][0], game[-1][1]
frames = len(game)
fam = collections.Counter(); cnt = collections.Counter(); busy = []
for s, e, label in kk:
    if s < t0 or e > t1 or e <= s:
        continue
    key = re.sub(r"[0-9a-f]+:(\d+x\d+)", r"\1", label) or "(unnamed)"
    fam[key] += e - s; cnt[key] += 1; busy.append((s, e))
busy.sort(); union = 0; cur_s = cur_e = None
for s, e in busy:
    if cur_e is None or s > cur_e:
        if cur_e is not None: union += cur_e - cur_s
        cur_s, cur_e = s, e
    else: cur_e = max(cur_e, e)
if cur_e is not None: union += cur_e - cur_s
total = sum(fam.values())
print(f"{frames} gameplay frames, {1000*(t1-t0)/frames:.1f} ms/frame wall, GPU busy union {1000*union/frames:.1f} ms/frame, sum of commits {1000*total/frames:.1f} ms/frame, {sum(cnt.values())/frames:.0f} commits/frame")
for key, t in fam.most_common(30):
    print(f"{1000*t/frames:7.2f} ms/frame {100*t/total:5.1f}%  {cnt[key]/frames:5.1f}/frame  {key[:150]}")
