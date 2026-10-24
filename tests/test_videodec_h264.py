#!/usr/bin/env python3
"""Exercise the guest Videodec HLE with a generated, synthetic H.264 frame."""

from __future__ import annotations

import shlex
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="gow3-videodec-") as temp_name:
        temp = Path(temp_name)
        bitstream = temp / "red-64x48.h264"
        executable = temp / "videodec-test"
        subprocess.run(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "color=c=red:s=64x48:r=1",
                "-frames:v",
                "1",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-tune",
                "zerolatency",
                "-pix_fmt",
                "yuv420p",
                "-f",
                "h264",
                str(bitstream),
            ],
            check=True,
        )
        pkg_flags = shlex.split(
            subprocess.check_output(
                ["pkg-config", "--cflags", "--libs", "libavcodec", "libavutil", "libswscale"],
                text=True,
            )
        )
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-Iruntime/core",
                "-Iruntime/modules/libSceVideodec",
                "tests/test_videodec_resource_info.c",
                "runtime/modules/libSceVideodec/ps4_videodec.c",
                *pkg_flags,
                "-Wl,-rpath,/opt/homebrew/opt/ffmpeg/lib",
                "-lpthread",
                "-o",
                str(executable),
            ],
            cwd=ROOT,
            check=True,
        )
        subprocess.run([str(executable), str(bitstream)], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
