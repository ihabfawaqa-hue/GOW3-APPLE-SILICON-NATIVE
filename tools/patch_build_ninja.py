#!/usr/bin/env python3
"""Compatibility wrapper for older scripts; build.ninja is now regenerated."""

from pathlib import Path
import argparse
import sys

from generate_build_ninja import render


def patch_ninja_file(ninja_path: str | Path) -> bool:
    output = Path(ninja_path).resolve()
    try:
        content = render(output.parent)
    except (FileNotFoundError, ValueError) as exc:
        print(f"build generation failed: {exc}", file=sys.stderr)
        return False
    old = output.read_text(encoding="utf-8") if output.exists() else None
    if old != content:
        with output.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        print(f"[+] Regenerated {output} from source files.")
    else:
        print(f"[+] {output} is current.")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build.ninja")
    args = parser.parse_args()
    raise SystemExit(0 if patch_ninja_file(args.path) else 2)
