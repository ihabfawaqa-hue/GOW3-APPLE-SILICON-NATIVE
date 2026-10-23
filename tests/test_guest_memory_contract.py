#!/usr/bin/env python3
"""Compile and run synthetic tests for the guest-memory lease contract."""

from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    compiler = shutil.which("clang")
    if not compiler:
        raise SystemExit("clang is required for the native guest-memory contract test")

    with tempfile.TemporaryDirectory(prefix="gow3-guest-memory-") as temp_dir:
        binary = Path(temp_dir) / "guest-memory-contract-test"
        command = [
            compiler,
            "-std=c11",
            "-D_DARWIN_C_SOURCE",
            "-I" + str(ROOT / "runtime/core"),
            "-I" + str(ROOT / "runtime/kernel/memory"),
            str(ROOT / "runtime/core/recomp_guest_memory.c"),
            str(ROOT / "tests/guest_memory_contract_test.c"),
            "-pthread",
            "-o",
            str(binary),
        ]
        built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        if built.returncode:
            raise SystemExit(built.stdout + built.stderr)
        run = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        if run.returncode:
            raise SystemExit(run.stdout + run.stderr)
        print(run.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
