#!/usr/bin/env python3
"""Exact peephole rewrites of the recompiled guest code (code_NNN.s -> code_NNN.opt.s).

The recompiler computes most guest addresses as
    mov x16, xA
    add x16, x16, #imm        (or sub)
which is one instruction:
    add x16, xA, #imm
It also emits vector self-moves (mov v0.16b, v0.16b, mov v0.s[0], v0.s[0]), which do nothing,
and zeroes v16 (the upper half of the guest's 256-bit register) after many vector operations:
a `movi v16.16b, #0` followed by another one with no read of v16 and no label, branch or call
in between is dead.

Only adjacent instructions with no label, directive or comment between them are merged, so
no branch can enter between the two. Both rewrites leave every register, flag and memory
value as before. Prints how many instructions were removed.
"""
import re
import sys

MOV = re.compile(r"^(\s+)mov (x\d+), (x\d+|sp)$")
ADDSUB = re.compile(r"^(\s+)(add|sub) (x\d+), (x\d+), (#\d+(?:, lsl #12)?)$")
SELF_MOVE = re.compile(r"^\s+mov (v\d+)\.(16b|[bhsd]\[\d+\]), \1\.\2$")
ZERO_V16 = "movi v16.16b, #0"
USES_V16 = re.compile(r"\b[vqdsbh]16\b")
CONTROL = re.compile(r"^(b|bl|br|blr|ret|cbz|cbnz|tbz|tbnz|b\.\w+)\b")


def optimize(lines):
    out = []
    removed = 0
    pending = None  # (index in out, destination, source) of a mov that may still merge
    zero_v16 = None  # index in out of a v16 zeroing not read yet in this straight-line run
    for line in lines:
        text = line.rstrip("\n")
        if SELF_MOVE.match(text):
            removed += 1
            continue  # keeps `pending`: removing a no-op keeps the two neighbours adjacent
        stripped = text.strip()
        if stripped == ZERO_V16:
            if zero_v16 is not None:
                out[zero_v16] = None  # zeroed again before any read
                removed += 1
            out.append(line if line.endswith("\n") else line + "\n")
            zero_v16 = len(out) - 1
            pending = None
            continue
        if (not stripped or stripped.startswith("//")) and zero_v16 is not None:
            pass  # comments do not end a straight-line run
        elif (stripped.endswith(":") or stripped.startswith(".") or CONTROL.match(stripped) or
              USES_V16.search(stripped)):
            zero_v16 = None
        m = ADDSUB.match(text)
        if m and pending and m.group(3) == m.group(4) == pending[1]:
            index, dest, source = pending
            out[index] = f"{m.group(1)}{m.group(2)} {dest}, {source}, {m.group(5)}\n"
            removed += 1
            pending = None
            continue
        mov = MOV.match(text)
        out.append(line if line.endswith("\n") else line + "\n")
        # The mov's destination must not be its source (mov x16, x16 is not emitted, but
        # merging it would still be exact) and sp only works as a source of add/sub.
        pending = (len(out) - 1, mov.group(2), mov.group(3)) if mov else None
    return [line for line in out if line is not None], removed


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: peephole_guest_code.py <code_NNN.s> <code_NNN.opt.s>", file=sys.stderr)
        return 2
    with open(sys.argv[1]) as stream:
        lines = stream.readlines()
    out, removed = optimize(lines)
    with open(sys.argv[2], "w") as stream:
        stream.writelines(out)
    print(f"{sys.argv[1]}: {removed} instructions removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
