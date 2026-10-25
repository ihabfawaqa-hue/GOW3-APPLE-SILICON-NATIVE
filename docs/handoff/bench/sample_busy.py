"""Per-thread running vs waiting sample counts from a macOS `sample` call tree.
Usage: sample_busy.py <sample.txt> [thread-substring-to-dump-hot-leaves]"""
import re, sys, collections
WAIT = re.compile(r"__psynch_cvwait|__psynch_mutexwait|semaphore_\w*wait|mach_msg|__semwait_signal|kevent|__select|"
                  r"nanosleep|__ulock_wait|__workq_kernreturn|__sigwait|read\s+\(in libsystem_kernel|__wait4|poll |"
                  r"__psynch_rw|swtch_pri|thread_switch|__accept|recvfrom|__pthread_kill|usleep|sched_yield")
lines = open(sys.argv[1]).read().split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith("Call graph:"))
end = next(i for i in range(start, len(lines)) if lines[i].startswith("Total number in stack"))
node = re.compile(r"^([ +!:|]*)(\d+) (.*)$")
threads = []
cur = None
stack = []
for l in lines[start + 1:end]:
    m = node.match(l)
    if not m:
        continue
    depth, count, sym = len(m.group(1)), int(m.group(2)), m.group(3)
    if "Thread_" in sym and depth <= 4:
        cur = {"name": sym.strip(), "total": count, "leaves": []}
        threads.append(cur); stack = [(depth, count, sym)]
        continue
    while stack and stack[-1][0] >= depth:
        stack.pop()
    stack.append((depth, count, sym))
    cur["leaves"].append((depth, count, sym, [s[2] for s in stack]))
rows = []
for t in threads:
    lv = t["leaves"]
    leaf_counts = []
    for i, (d, c, s, path) in enumerate(lv):
        child = sum(c2 for d2, c2, s2, p2 in lv[i + 1:] if d2 > d and all(x[0] > d for x in [(d2,)]) and False)
        # self time = count minus direct children counts
        kids = 0
        for d2, c2, s2, p2 in lv[i + 1:]:
            if d2 <= d: break
            if len(p2) == len(path) + 1: kids += c2
        if c - kids > 0:
            leaf_counts.append((c - kids, s, path))
    wait = sum(c for c, s, p in leaf_counts if WAIT.search(s))
    run = sum(c for c, s, p in leaf_counts) - wait
    rows.append((run, wait, t["name"], leaf_counts))
rows.sort(reverse=True)
for run, wait, name, _ in rows[:30]:
    print(f"{run:5d} run {wait:5d} wait  {name[:80]}")
if len(sys.argv) > 2:
    for run, wait, name, lc in rows:
        if sys.argv[2] in name:
            agg = collections.Counter()
            for c, s, p in lc:
                if not WAIT.search(s):
                    agg[re.sub(r"\s+\(in .*|  \+ \d+.*|\s+\[0x.*", "", s)[:110]] += c
            print("--", name)
            for s, c in agg.most_common(25): print(f"  {c:5d} {s}")
