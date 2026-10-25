#!/usr/bin/env python3
"""Exercise the guest Videodec HLE with a generated, synthetic H.264 frame."""

from __future__ import annotations

import shlex
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="gow3-videodec-") as temp_name:
        temp = Path(temp_name)
        bitstream = temp / "red-64x48.h264"
        reordered_stream = temp / "reordered-64x48.ts"
        packet_bundle = temp / "reordered-packets.bin"
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
        subprocess.run(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "testsrc2=size=64x48:rate=12:duration=1",
                "-frames:v",
                "12",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-pix_fmt",
                "yuv420p",
                "-bf",
                "3",
                "-g",
                "12",
                "-x264-params",
                "scenecut=0:min-keyint=12",
                "-mpegts_flags",
                "+resend_headers",
                "-f",
                "mpegts",
                str(reordered_stream),
            ],
            check=True,
        )
        packet_doc = json.loads(
            subprocess.check_output(
                [
                    "ffprobe",
                    "-v",
                    "error",
                    "-select_streams",
                    "v:0",
                    "-show_packets",
                    "-show_data",
                    "-of",
                    "json",
                    str(reordered_stream),
                ],
                text=True,
            )
        )
        packets = packet_doc["packets"]
        assert len(packets) == 12
        assert len({packet["pts"] for packet in packets}) == len(packets)
        assert any(packet["pts"] != packet["dts"] for packet in packets)
        with packet_bundle.open("wb") as bundle:
            bundle.write(struct.pack("<8sI", b"GOW3VD1\0", len(packets)))
            for packet in packets:
                data = bytearray()
                for line in packet["data"].splitlines():
                    match = re.match(r"^[0-9a-fA-F]{8}:\s*(.*)$", line)
                    if match:
                        hex_data = match.group(1).split("  ", 1)[0]
                        data.extend(bytes.fromhex("".join(hex_data.split())))
                assert len(data) == int(packet["size"])
                bundle.write(
                    struct.pack(
                        "<qqI",
                        int(packet["pts"]),
                        int(packet["dts"]),
                        len(data),
                    )
                )
                bundle.write(data)
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
        subprocess.run(
            [str(executable), str(bitstream), str(packet_bundle)],
            cwd=ROOT,
            check=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
