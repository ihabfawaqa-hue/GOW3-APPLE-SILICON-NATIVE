#!/usr/bin/env python3
"""Check deterministic source-driven Ninja generation and unique object ownership."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BASE_DIR = Path(__file__).resolve().parents[1]
NINJA_FILE = BASE_DIR / "build.ninja"
GENERATOR = BASE_DIR / "tools/generate_build_ninja.py"


def build_lines(text):
    return [line for line in text.splitlines() if line.startswith("build ") and ":" in line]


def test_no_duplicate_outputs():
    print("[1] Checking that every Ninja output has exactly one owner...")
    generated = {}
    for number, line in enumerate(build_lines(NINJA_FILE.read_text(encoding="utf-8")), 1):
        targets = line[len("build "):line.index(":")].split()
        for target in targets:
            generated.setdefault(target, []).append((number, line))
    duplicates = {target: rules for target, rules in generated.items() if len(rules) != 1}
    assert not duplicates, f"duplicate Ninja outputs: {duplicates}"
    print(f"    [+] {len(generated)} unique build outputs")


def test_language_rules_and_bridge_link():
    print("[2] Checking source-language rules and the single bridge object...")
    lines = build_lines(NINJA_FILE.read_text(encoding="utf-8"))
    bridge = [line for line in lines if line.startswith("build gpu/gow3gpu_bridge.o:")]
    assert len(bridge) == 1 and ": compile_cpp gpu/gow3gpu_bridge.cpp" in bridge[0]
    app = [line for line in lines if line.startswith("build ps4_app:")]
    assert len(app) == 1 and app[0].split().count("gpu/gow3gpu_bridge.o") == 1
    rules = {".c": "compile_c", ".s": "compile_s", ".m": "compile_m",
             ".mm": "compile_mm", ".cpp": "compile_cpp"}
    for line in lines:
        lhs, rhs = line.split(":", 1)
        source = rhs.split()[-1]
        if source.endswith(tuple(rules)) and "build/gcn/" not in lhs:
            suffix = Path(source).suffix
            assert f": {rules[suffix]} " in line, f"wrong rule for {source}: {line}"
    assert any("compile_cpp23 runtime/modules/libSceGnmDriver/ps4_gcn_translate.cpp" in line
               for line in lines)
    print("    [+] C, assembly, Objective-C, Objective-C++, C++ and C++23 routes are explicit")
    print("    [+] gow3gpu_bridge.o has one compile rule and one link entry")


def test_generator_idempotence():
    print("[3] Generating two independent build files and comparing them byte-for-byte...")
    with tempfile.TemporaryDirectory(prefix="gow3-buildgen-") as temp_dir:
        first = Path(temp_dir) / "first.ninja"
        second = Path(temp_dir) / "second.ninja"
        for output in (first, second):
            run = subprocess.run(
                [sys.executable, str(GENERATOR), "--root", str(BASE_DIR), "--output", str(output)],
                capture_output=True, text=True,
            )
            assert run.returncode == 0, run.stderr
        assert first.read_bytes() == second.read_bytes()
        assert first.read_bytes() == NINJA_FILE.read_bytes(), "checked-in graph differs from generated graph"
    print("    [+] repeated generation is bit-for-bit stable")


def test_ninja_parse():
    print("[4] Asking Ninja to parse and dry-run the app target...")
    ninja = shutil.which("ninja")
    if not ninja:
        print("    [*] Ninja unavailable; skipped parser check")
        return
    result = subprocess.run([ninja, "-C", str(BASE_DIR), "-n", "ps4_app"],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "multiple rules generate" not in result.stderr
    print("    [+] Ninja parsed ps4_app without graph errors")


if __name__ == "__main__":
    test_no_duplicate_outputs()
    test_language_rules_and_bridge_link()
    test_generator_idempotence()
    test_ninja_parse()
