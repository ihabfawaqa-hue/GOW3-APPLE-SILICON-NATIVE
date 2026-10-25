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
no branch can enter between the two. These rewrites leave every register, flag and memory
value as before.

Guest loads and stores of 2, 4 and 8 bytes are emitted as (S = x7, or x6 where x7 is in use)
    add S, x16, #size-1 ; eor S, S, x16 ; tbz S, #4, L_tso_ok_N    crosses 16 bytes?
    ldr ; dmb ishld   (or dmb ish ; str)                         crossing: plain access
    b L_tso_done_N
  L_tso_ok_N:
    add S, x28, x16 ; ldapr/stlr [S]                             acquire load, release store
  L_tso_done_N:
ldapr and stlr fault (SIGBUS) on an access crossing 16 bytes, which is why the check exists.
Only the second half is kept: an access that crosses faults, and the runtime
(runtime/core/tso_crossing.c) runs the first half for it. The ordering of every access stays
as before; S is never read after the block (its value already differed between the halves).
Every access of a function containing a guest instruction listed in
tools/tso_crossing_sites.txt (the runtime logs the sites that fault) keeps its check: the
crossing accesses found so far sit in functions that handle packed data (command-buffer
writers storing 8 bytes at 4-byte alignment), whose other accesses are likely to cross too.
--keep-tso-checks keeps every check. Each rewritten
access is labelled and listed with its guest address in __DATA,__gow3_tso for that log.

Some functions get a hand-written body instead (NATIVE_BODIES), with the same entry, exit state
and memory ordering.

Prints how many instructions were removed.
"""
import os
import re
import sys

MOV = re.compile(r"^(\s+)mov (x\d+), (x\d+|sp)$")
ADDSUB = re.compile(r"^(\s+)(add|sub) (x\d+), (x\d+), (#\d+(?:, lsl #12)?)$")
SELF_MOVE = re.compile(r"^\s+mov (v\d+)\.(16b|[bhsd]\[\d+\]), \1\.\2$")
ZERO_V16 = "movi v16.16b, #0"
USES_V16 = re.compile(r"\b[vqdsbh]16\b")
CONTROL = re.compile(r"^(b|bl|br|blr|ret|cbz|cbnz|tbz|tbnz|b\.\w+)\b")
GUEST_ADDRESS = re.compile(r"^\s*// (0x[0-9a-f]+):")
FUNCTION = re.compile(r"^_fn_(0x[0-9a-f]+):")
TSO_SLOW = {
    ("load", "dmb ishld"): re.compile(r"^(ldr|ldrh|ldrsw|ldrsh) +[wx](\d+|zr), \[x28, (x1[67])\]$"),
    ("store", "dmb ish"): re.compile(r"^(str|strh) +[wx](\d+|zr), \[x28, (x1[67])\]$"),
}
TSO_FAST = re.compile(r"^(ldapr|ldaprh|stlr|stlrh) [wx](\d+|zr), \[(x[67])\]$")
# fn_0x1a5d10 is God of War III's job wait: `loop: cmp dword [rdi], 0 / jne loop`, about 16% of
# the main thread in gameplay. Spinning keeps a core at full power, which a fanless Mac pays for
# in clock speed. This waits the ARM way: an exclusive load (acquire, as the original's ldapr, and
# arming the monitor) and wfe, which sleeps the core until another core writes the flag's line.
# A flag that is not 4-byte aligned (ldaxr would fault) keeps the original loop. Exit state as
# the original: w17 = 0, x7 = the flag's host address (aligned case), flags of `cmp w17, #0`.
NATIVE_BODIES = {
    "fn_0x1a5d10": """\
    str x30, [sp, #-16]!
    tst x24, #3
    b.ne L_gow3_wait_plain
    add x7, x28, x24
L_gow3_wait_load:
    ldaxr w17, [x7]
    cbz w17, L_gow3_wait_done
    wfe
    b L_gow3_wait_load
L_gow3_wait_plain:
    mov x16, x24
    add x7, x16, #3
    eor x7, x7, x16
    tbz x7, #4, L_gow3_wait_ordered
    ldr w17, [x28, x16]
    dmb ishld
    cbz w17, L_gow3_wait_done
    b L_gow3_wait_yield
L_gow3_wait_ordered:
    add x7, x28, x16
    ldapr w17, [x7]
    cbz w17, L_gow3_wait_done
L_gow3_wait_yield:
    yield
    b L_gow3_wait_plain
L_gow3_wait_done:
    clrex
    cmp w17, #0
    ldr x16, [x28, x26]
    add x26, x26, #8
    ldr x30, [sp], #16
    ret
""",
}


def replace_bodies(lines):
    """Swaps in NATIVE_BODIES: from a function's label to the next function's .globl."""
    out, skipping = [], False
    for line in lines:
        m = re.match(r"^_(fn_0x[0-9a-f]+):$", line.strip())
        if skipping and line.startswith(".globl "):
            skipping = False
        if m and m.group(1) in NATIVE_BODIES:
            out += [line, NATIVE_BODIES[m.group(1)], "\n"]
            skipping = True
            continue
        if not skipping:
            out.append(line)
    return out


SITES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tso_crossing_sites.txt")


def load_sites(path=SITES):
    """Guest instruction addresses whose accesses keep the crossing check."""
    if not os.path.exists(path):
        return set()
    with open(path) as stream:
        return {int(line.split("#")[0], 16) for line in stream if line.split("#")[0].strip()}


def tso_block(lines, i):
    """(length, kept lines) of the guarded access starting at lines[i], or None."""
    t = [line.strip() for line in lines[i:i + 11]]
    if len(t) < 10:
        return None
    m = re.fullmatch(r"add (x[67]), (x1[67]), #\d+", t[0])
    if not m:
        return None
    scratch, address = m.groups()
    ok = re.fullmatch(rf"tbz {scratch}, #4, (L_tso_ok_\d+)", t[2])
    if t[1] != f"eor {scratch}, {scratch}, {address}" or not ok:
        return None
    label = ok.group(1)
    done = label.replace("_ok_", "_done_")
    slow = (t[3], t[4]) if t[4].startswith("dmb") else (t[4], t[3])
    kind = next((k for (k, barrier), pattern in TSO_SLOW.items()
                 if slow[1] == barrier and pattern.match(slow[0]) and
                 pattern.match(slow[0]).group(3) == address), None)
    fast = TSO_FAST.match(t[8])
    if (kind is None or t[5] != f"b {done}" or t[6] != f"{label}:" or
            t[7] != f"add {scratch}, x28, {address}" or not fast or fast.group(3) != scratch or
            fast.group(1).startswith("ld") != (kind == "load") or
            fast.group(2) == scratch[1:]):
        return None
    end = 9 if t[9] == f"{done}:" else 10
    if end == 10 and not (re.fullmatch(r"sxt[hw] x\d+, w\d+", t[9]) and t[10] == f"{done}:"):
        return None
    return end + 1, [lines[i + 7], label.replace("L_tso_ok_", "Ltso_site_") + ":\n"] + lines[i + 8:i + end]


def drop_tso_checks(lines, keep_sites):
    """Rewrites the guarded accesses; returns (lines, instructions removed)."""
    keep_functions, function = set(), None
    for line in lines:
        m = FUNCTION.match(line)
        if m:
            function = m.group(1)
        m = GUEST_ADDRESS.match(line)
        if m and int(m.group(1), 16) in keep_sites:
            keep_functions.add(function)
    out, sites, removed = [], [], 0
    guest = function = None
    i = 0
    while i < len(lines):
        m = FUNCTION.match(lines[i])
        if m:
            function = m.group(1)
        m = GUEST_ADDRESS.match(lines[i])
        if m:
            guest = int(m.group(1), 16)
        block = tso_block(lines, i) if function not in keep_functions else None
        if block is None:
            out.append(lines[i])
            i += 1
            continue
        length, kept = block
        out += kept
        sites.append((kept[1].strip()[:-1], guest))
        removed += 6  # add, eor, tbz, the slow access and barrier, b
        i += length
    if sites:
        out.append("\n.section __DATA,__gow3_tso,regular\n.p2align 3\n")
        out += [f".quad {label}, {guest:#x}\n" for label, guest in sites]
    return out, removed


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
    args = sys.argv[1:]
    keep_all = "--keep-tso-checks" in args
    args = [arg for arg in args if arg != "--keep-tso-checks"]
    if len(args) != 2:
        print("usage: peephole_guest_code.py [--keep-tso-checks] <code_NNN.s> <code_NNN.opt.s>",
              file=sys.stderr)
        return 2
    with open(args[0]) as stream:
        lines = [line if line.endswith("\n") else line + "\n" for line in stream]
    lines = replace_bodies(lines)
    tso_removed = 0
    if not keep_all:
        lines, tso_removed = drop_tso_checks(lines, load_sites())
    out, removed = optimize(lines)
    with open(args[1], "w") as stream:
        stream.writelines(out)
    print(f"{args[0]}: {removed + tso_removed} instructions removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
