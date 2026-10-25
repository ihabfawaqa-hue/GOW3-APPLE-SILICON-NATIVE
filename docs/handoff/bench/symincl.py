"""Inclusive sample counts of symbols per thread from a macOS `sample` call tree.
Usage: symincl.py <sample.txt> <thread-substring> <sym-substring>...
Counts the outermost occurrence of each symbol in every stack (no double counting of recursion)."""
import re, sys
lines = open(sys.argv[1]).read().split("\n")
tsub, syms = sys.argv[2], sys.argv[3:]
start = next(i for i, l in enumerate(lines) if l.startswith("Call graph:"))
end = next(i for i in range(start, len(lines)) if lines[i].startswith("Total number in stack"))
node = re.compile(r"^([ +!:|]*)(\d+) (.*)$")
total = None; active = False; stack = []; counts = {s: 0 for s in syms}
for l in lines[start + 1:end]:
    m = node.match(l)
    if not m: continue
    depth, count, sym = len(m.group(1)), int(m.group(2)), m.group(3)
    if "Thread_" in sym and depth <= 4:
        active = tsub in sym; stack = []
        if active: total = (total or 0) + count
        continue
    if not active: continue
    while stack and stack[-1][0] >= depth: stack.pop()
    for s in syms:
        if s in sym and not any(s in x[1] for x in stack):
            counts[s] += count
    stack.append((depth, sym))
print(f"thread '{tsub}': {total} samples")
for s in syms: print(f"  {counts[s]:6d} {100*counts[s]/max(total,1):5.1f}%  {s}")
