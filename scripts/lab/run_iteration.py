#!/usr/bin/env python3
"""Bounded build, launch, capture, and report loop for the local GOW3 port.

This harness records observations from the real launcher and screenshots. It does
not infer successful shader translation, GPU replay, audio, or authentic visuals
from synthetic fixtures or activity counters.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import platform
import re
import signal
import struct
import subprocess
import sys
import time
import zlib
from collections import Counter
from pathlib import Path
from audio_metrics import inspect_float_wav, parse_ajm_decode_log, parse_audioout_log

try:
    from PIL import Image as _PILImage
except ImportError:
    _PILImage = None

BASE_DIR = Path(__file__).resolve().parents[2]
DEFAULT_RUNS_DIR = Path.home() / "Library/Application Support/GOW3Lab/runs"
RUNS_DIR = Path(os.environ.get("GOW3_LAB_RUNS", DEFAULT_RUNS_DIR)).expanduser().resolve()
DEFAULT_ASSETS = Path("/Volumes/1TB/gow3_cusa01623_extracted")
SOURCE_FILES = [
    "build.ninja", "main.c", "Launch_GOW3.command", "fix_and_resume.sh",
    "runtime/core/recomp_runtime.c",
    "runtime/modules/libSceAjm/ps4_ajm.c",
    "tools/gow3_lab.py", "tools/gow3_gpu_replay.py", "tools/gow3_shader_tool.py",
    "tools/generate_build_ninja.py", "tools/patch_build_ninja.py", "build_and_run.sh",
    "gpu/gow3gpu.h", "gpu/gow3gpu_bridge.cpp", "gpu/VENDOR.txt", "gpu/gow3_capture.h",
    "runtime/modules/libSceGnmDriver/ps4_gnm_metal.mm",
    "runtime/modules/libSceGnmDriver/ps4_gcn_recompiler.h",
    "runtime/modules/libSceGnmDriver/ps4_gnmdriver.c",
    "runtime/modules/libSceVideoOut/ps4_metal_screen.m",
    "runtime/modules/libSceVideoOut/ps4_videoout.c",
    "runtime/core/gow3gpu_runtime_loader.c",
    "runtime/core/gow3gpu_runtime_loader.h",
    "third_party/gow3gpu/gpu/shadps4/core/libraries/videoout/driver.cpp",
    "third_party/gow3gpu/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp",
    "third_party/gow3gpu/gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp",
    "runtime/modules/libSceVideodec/ps4_videodec.c",
    "runtime/modules/libSceVideodec/ps4_videodec.h",
    "scripts/lab/run_iteration.py",
    "scripts/lab/audio_metrics.py",
    "tests/test_audio_metrics.py",
    "tests/test_texture_descriptor_decoding.py",
    "tests/test_authenticity_classifier.py",
    "tests/test_videodec_h264.py",
    "tests/test_videodec_resource_info.c",
    "tests/test_shader_telemetry_truth.py",
]
PRESENT_RE = re.compile(
    r"present #(\d+): addr=(0x[0-9a-fA-F]+) (\d+)x(\d+) .*?"
    r"decoded=(\d+), draws_in=(\d+), draws_enc=(\d+), vb_bound=(\d+), "
    r"ib_bound=(\d+), tex_bound=(\d+), disp=(\d+), pso=(\d+), rt0=(0x[0-9a-fA-F]+)"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def git_revision() -> str | None:
    proc = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=BASE_DIR, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    )
    return proc.stdout.strip() if proc.returncode == 0 else None


def source_fingerprints() -> dict[str, str]:
    result = {}
    for rel in SOURCE_FILES:
        path = BASE_DIR / rel
        if path.is_file():
            result[rel] = sha256(path)
    return result


def process_rss_mb(pid: int) -> float | None:
    try:
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=2,
        )
        value = result.stdout.strip().splitlines()
        return int(value[0]) / 1024 if result.returncode == 0 and value else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def stop_owned_process_group(proc: subprocess.Popen[bytes]) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


def run_bounded(
    command: list[str], cwd: Path, env: dict[str, str], stdout_path: Path,
    stderr_path: Path | None, timeout_s: float, no_progress_s: float,
    max_log_bytes: int, max_rss_mb: int, progress_files: Path | None = None,
    capture_dirs: list[Path] | None = None, max_capture_bytes: int = 512 * 1024 * 1024,
) -> dict[str, object]:
    start = time.monotonic()
    result: dict[str, object] = {
        "command": command,
        "status": "NOT_STARTED",
        "exit_code": None,
        "termination_reason": None,
        "duration_seconds": 0.0,
        "max_rss_mb": None,
        "max_capture_bytes": 0,
    }
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_handle = open(stderr_path, "wb") if stderr_path else subprocess.STDOUT
    try:
        with stdout_path.open("wb") as stdout_handle:
            proc = subprocess.Popen(
                command, cwd=cwd, env=env, stdout=stdout_handle,
                stderr=stderr_handle, start_new_session=True,
            )
            result["status"] = "RUNNING"
            last_activity = time.monotonic()
            previous_signature: tuple[int, int, int] | None = None
            max_rss = 0.0
            reason = None
            while proc.poll() is None:
                now = time.monotonic()
                stdout_size = stdout_path.stat().st_size if stdout_path.exists() else 0
                stderr_size = stderr_path.stat().st_size if stderr_path and stderr_path.exists() else 0
                file_count = len(list(progress_files.glob("frame_*.png"))) if progress_files else 0
                signature = (stdout_size, stderr_size, file_count)
                if signature != previous_signature:
                    last_activity = now
                    previous_signature = signature

                rss = process_rss_mb(proc.pid)
                if rss is not None:
                    max_rss = max(max_rss, rss)
                    result["max_rss_mb"] = round(max_rss, 1)

                capture_bytes = 0
                for directory in capture_dirs or []:
                    if directory.exists():
                        capture_bytes += sum(
                            item.stat().st_size for item in directory.rglob("*")
                            if item.is_file()
                        )
                result["max_capture_bytes"] = max(int(result["max_capture_bytes"]), capture_bytes)

                if stdout_size + stderr_size > max_log_bytes:
                    reason = "log_size_limit"
                elif capture_bytes > max_capture_bytes:
                    reason = "capture_size_limit"
                elif rss is not None and rss > max_rss_mb:
                    reason = "memory_limit"
                elif now - start >= timeout_s:
                    reason = "deadline"
                elif no_progress_s > 0 and now - last_activity >= no_progress_s:
                    reason = "no_progress"

                if reason:
                    stop_owned_process_group(proc)
                    break
                time.sleep(0.5)

            exit_code = proc.poll()
            result["exit_code"] = exit_code
            result["termination_reason"] = reason or "process_exit"
            result["status"] = "TERMINATED_BY_LIMIT" if reason else ("EXITED" if exit_code == 0 else "CRASHED")
            result["duration_seconds"] = round(time.monotonic() - start, 3)
            return result
    except OSError as exc:
        result["status"] = "START_FAILED"
        result["error"] = str(exc)
        result["duration_seconds"] = round(time.monotonic() - start, 3)
        return result
    except KeyboardInterrupt:
        if "proc" in locals():
            stop_owned_process_group(proc)
            result["exit_code"] = proc.poll()
        result["status"] = "INTERRUPTED"
        result["termination_reason"] = "user_interrupt"
        result["duration_seconds"] = round(time.monotonic() - start, 3)
        return result
    finally:
        if stderr_path and not stderr_handle.closed:
            stderr_handle.close()


def first_error_excerpt(path: Path) -> str | None:
    if not path.exists():
        return None
    patterns = re.compile(r"(?i)(error:|undefined symbols|duplicate.*(rule|output)|ninja: build stopped|fatal error)")
    lines = path.read_text(errors="replace").splitlines()
    for index, line in enumerate(lines):
        if patterns.search(line):
            return "\n".join(lines[max(0, index - 2): min(len(lines), index + 4)])
    return "\n".join(lines[-12:]) if lines else None


def assess_frame(
    sampled_pixels: int, black_ratio: float, unique_bins: int, dominant_ratio: float,
    luminance_stddev: float, left_black_ratio: float, right_black_ratio: float,
) -> str:
    """Describe coarse output without treating color statistics as authenticity."""
    if sampled_pixels == 0 or black_ratio > 0.98:
        return "NO_FRAME_OR_BLACK"

    # A few flat colors plus a strong left/right black boundary is a useful
    # malformed-geometry signal for this renderer. It remains only a candidate.
    if (
        unique_bins <= 16
        and luminance_stddev >= 12.0
        and black_ratio >= 0.15
        and max(left_black_ratio, right_black_ratio) >= 0.90
        and abs(left_black_ratio - right_black_ratio) >= 0.25
    ):
        return "MALFORMED_POLYGON_CANDIDATE"
    if unique_bins < 32 or dominant_ratio > 0.65 or luminance_stddev < 15.0:
        return "FLAT_CLEAR_OR_FALLBACK"
    return "NONTRIVIAL_RENDER_UNVERIFIED"


def read_png(path: Path) -> dict[str, object]:
    """Decode a screenshot, using Pillow for fast downsampled metrics when installed."""
    if _PILImage is not None:
        with _PILImage.open(path) as source:
            width, height = source.size
            rgb = source.convert("RGB")
            resampling = _PILImage.Resampling.NEAREST
            sampled_image = rgb.resize((min(256, width), min(144, height)), resampling)
            grid_image = rgb.resize((64, 36), resampling)
            pixels = list(sampled_image.getdata())
            grid = list(grid_image.getdata())

        bins: Counter[int] = Counter()
        black = gray = left_black = right_black = 0
        left_sampled = right_sampled = 0
        lum_sum = lum_sq_sum = 0.0
        sample_width, sample_height = sampled_image.size
        for index, (r, g, b) in enumerate(pixels):
            bins[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)] += 1
            is_black = r <= 8 and g <= 8 and b <= 8
            black += int(is_black)
            if index % sample_width < sample_width // 2:
                left_black += int(is_black)
                left_sampled += 1
            else:
                right_black += int(is_black)
                right_sampled += 1
            gray += int(max(r, g, b) - min(r, g, b) <= 4)
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            lum_sum += lum
            lum_sq_sum += lum * lum

        sampled = len(pixels)
        dominant_bin, dominant_count = max(bins.items(), key=lambda item: item[1]) if bins else (0, 0)
        mean = lum_sum / sampled if sampled else 0.0
        variance = max(0.0, lum_sq_sum / sampled - mean * mean) if sampled else 0.0
        left_ratio = left_black / left_sampled if left_sampled else 0.0
        right_ratio = right_black / right_sampled if right_sampled else 0.0
        metrics = {
            "width": width,
            "height": height,
            "sampled_pixels": sampled,
            "unique_color_bins_12bit": len(bins),
            "dominant_color_bin_ratio": round(dominant_count / sampled, 6) if sampled else 0.0,
            "dominant_color_bin_rgb4": [((dominant_bin >> 8) & 15), ((dominant_bin >> 4) & 15), (dominant_bin & 15)],
            "black_pixel_ratio_sampled": round(black / sampled, 6) if sampled else 0.0,
            "grayscale_pixel_ratio_sampled": round(gray / sampled, 6) if sampled else 0.0,
            "luminance_stddev_sampled": round(math.sqrt(variance), 3),
            "left_half_black_ratio_sampled": round(left_ratio, 6),
            "right_half_black_ratio_sampled": round(right_ratio, 6),
            "sample_grid_rgb": [list(pixel) for pixel in grid],
        }
        metrics["frame_assessment"] = assess_frame(
            sampled, black / sampled if sampled else 0.0, len(bins),
            dominant_count / sampled if sampled else 0.0, math.sqrt(variance),
            left_ratio, right_ratio,
        )
        metrics["visual_authenticity"] = "UNVERIFIED"
        return metrics

    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    while pos + 12 <= len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += length + 12
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise ValueError(f"unsupported PNG encoding: bit_depth={bit_depth}, color_type={color_type}, interlace={interlace}")

    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(compressed)
    expected = height * (stride + 1)
    if len(raw) != expected:
        raise ValueError(f"PNG data length mismatch: expected {expected}, got {len(raw)}")

    bins: Counter[int] = Counter()
    sampled = 0
    black = gray = left_black = right_black = 0
    left_sampled = right_sampled = 0
    lum_sum = lum_sq_sum = 0.0
    stride_x = max(1, width // 256)
    stride_y = max(1, height // 144)
    y_grid = {min(height - 1, int((i + 0.5) * height / 36)): i for i in range(36)}
    x_grid = {min(width - 1, int((i + 0.5) * width / 64)): i for i in range(64)}
    grid = [[None] * 64 for _ in range(36)]
    previous = bytearray(stride)
    offset = 0

    def paeth(a: int, b: int, c: int) -> int:
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

    for y in range(height):
        filt = raw[offset]
        source = raw[offset + 1:offset + 1 + stride]
        offset += stride + 1
        row = bytearray(stride)
        for i, value in enumerate(source):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if filt == 1:
                value += left
            elif filt == 2:
                value += up
            elif filt == 3:
                value += (left + up) // 2
            elif filt == 4:
                value += paeth(left, up, upper_left)
            elif filt != 0:
                raise ValueError(f"unsupported PNG filter {filt}")
            row[i] = value & 0xFF

        if y % stride_y == 0:
            for x in range(0, width, stride_x):
                idx = x * channels
                r, g, b = row[idx], row[idx + 1], row[idx + 2]
                bins[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)] += 1
                sampled += 1
                is_black = r <= 8 and g <= 8 and b <= 8
                black += int(is_black)
                if x < width // 2:
                    left_black += int(is_black)
                    left_sampled += 1
                else:
                    right_black += int(is_black)
                    right_sampled += 1
                gray += int(max(r, g, b) - min(r, g, b) <= 4)
                lum = 0.299 * r + 0.587 * g + 0.114 * b
                lum_sum += lum
                lum_sq_sum += lum * lum
        if y in y_grid:
            gy = y_grid[y]
            for x, gx in x_grid.items():
                idx = x * channels
                grid[gy][gx] = [row[idx], row[idx + 1], row[idx + 2]]
        previous = row

    dominant_bin, dominant_count = max(bins.items(), key=lambda item: item[1]) if bins else (0, 0)
    mean = lum_sum / sampled if sampled else 0.0
    variance = max(0.0, lum_sq_sum / sampled - mean * mean) if sampled else 0.0
    metrics = {
        "width": width,
        "height": height,
        "sampled_pixels": sampled,
        "unique_color_bins_12bit": len(bins),
        "dominant_color_bin_ratio": round(dominant_count / sampled, 6) if sampled else 0.0,
        "dominant_color_bin_rgb4": [((dominant_bin >> 8) & 15), ((dominant_bin >> 4) & 15), (dominant_bin & 15)],
        "black_pixel_ratio_sampled": round(black / sampled, 6) if sampled else 0.0,
        "grayscale_pixel_ratio_sampled": round(gray / sampled, 6) if sampled else 0.0,
        "luminance_stddev_sampled": round(math.sqrt(variance), 3),
        "left_half_black_ratio_sampled": round(left_black / max(1, left_sampled), 6),
        "right_half_black_ratio_sampled": round(right_black / max(1, right_sampled), 6),
        "sample_grid_rgb": [pixel for row in grid for pixel in row],
    }
    metrics["frame_assessment"] = assess_frame(
        sampled, black / sampled if sampled else 0.0, len(bins),
        dominant_count / sampled if sampled else 0.0,
        math.sqrt(variance),
        left_black / left_sampled if left_sampled else 0.0,
        right_black / right_sampled if right_sampled else 0.0,
    )
    metrics["visual_authenticity"] = "UNVERIFIED"
    return metrics


def frame_files(directory: Path) -> list[Path]:
    return sorted(directory.glob("frame_*.png")) if directory.exists() else []


def write_rgba_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    """Write a bounded RGBA8 image without requiring Pillow."""
    if len(rgba) != width * height * 4:
        raise ValueError(f"RGBA byte count does not match {width}x{height}")

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    scanlines = b"".join(
        b"\x00" + rgba[y * width * 4:(y + 1) * width * 4]
        for y in range(height)
    )
    payload = (b"\x89PNG\r\n\x1a\n" +
               chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(scanlines, level=3)) +
               chunk(b"IEND", b""))
    path.write_bytes(payload)


def convert_candidate_frame_dumps(run_dir: Path, text: str) -> list[dict[str, object]]:
    """Convert only known 8-bit Vulkan output formats into screenshot PNGs."""
    converted: list[dict[str, object]] = []
    for match in re.finditer(
        r"\[gow3gpu-frame\] raw file=([^\s]+) format=([^\s]+) width=(\d+) height=(\d+)"
        r"(?: storage=([^\s]+))?",
        text,
    ):
        filename, vk_format = match.group(1), match.group(2)
        width, height = int(match.group(3)), int(match.group(4))
        source = run_dir / "candidate-dump" / filename
        record: dict[str, object] = {
            "raw_file": str(source.relative_to(run_dir)),
            "format": vk_format,
            "storage_format": match.group(5) or vk_format,
            "width": width,
            "height": height,
            "status": "NOT_CONVERTED",
        }
        try:
            raw = source.read_bytes()
            if len(raw) != width * height * 4:
                raise ValueError(f"raw image has {len(raw)} bytes, expected {width * height * 4}")
            lower_format = vk_format.lower()
            if "b8g8r8a8" in lower_format:
                rgba = bytearray(raw)
                rgba[0::4], rgba[2::4] = raw[2::4], raw[0::4]
                pixel_order = "BGRA8 converted to RGBA8"
            elif any(token in lower_format for token in ("r8g8b8a8", "a8b8g8r8")):
                rgba = raw
                pixel_order = "RGBA8"
            else:
                raise ValueError(f"unsupported output image format {vk_format}")
            # The presenter samples display buffers with alpha swizzled to one, so a zero
            # alpha in guest memory is not transparency on screen: write opaque PNGs.
            rgba = bytearray(rgba)
            rgba[3::4] = b"\xff" * (width * height)
            prefix = "frame_candidate_output" if filename.startswith("output_") else "frame_candidate_display"
            target = run_dir / "frames" / f"{prefix}_{source.stem}.png"
            write_rgba_png(target, width, height, bytes(rgba))
            record.update({
                "png_file": str(target.relative_to(run_dir)),
                "png_sha256": sha256(target),
                "pixel_order": pixel_order,
                "status": "CONVERTED",
            })
        except Exception as exc:
            record["error"] = str(exc)
        converted.append(record)
    return converted


def previous_frame(exclude_dir: Path) -> Path | None:
    candidates = list(RUNS_DIR.glob("*/frames/*.png")) + list((BASE_DIR / "screenshots").glob("frame_*.png"))
    candidates = [p for p in candidates if p.is_file() and exclude_dir not in p.parents]
    return max(candidates, key=lambda p: p.stat().st_mtime_ns) if candidates else None


def analyze_frames(paths: list[Path], run_dir: Path) -> tuple[list[dict[str, object]], dict[str, object]]:
    records = []
    decoded = {}
    for path in paths:
        try:
            metrics = read_png(path)
            decoded[path] = metrics
            records.append({
                "file": str(path.relative_to(run_dir)),
                "sha256": sha256(path),
                "bytes": path.stat().st_size,
                **metrics,
            })
        except Exception as exc:
            records.append({"file": str(path.relative_to(run_dir)), "sha256": sha256(path), "analysis_error": str(exc), "visual_authenticity": "UNVERIFIED"})

    comparison: dict[str, object] = {"status": "NO_CURRENT_FRAME"}
    if decoded:
        current = max(decoded, key=lambda p: p.stat().st_mtime_ns)
        prior_frames = list(RUNS_DIR.glob(f"*/frames/{current.name}"))
        prior_frames += list((BASE_DIR / "screenshots").glob(current.name))
        prior_frames = [p for p in prior_frames if p.is_file() and run_dir not in p.parents]
        reference = max(prior_frames, key=lambda p: p.stat().st_mtime_ns) if prior_frames else previous_frame(run_dir)
        comparison = {
            "status": "REFERENCE_FOUND" if reference else "NO_REFERENCE",
            "current_frame": str(current.relative_to(run_dir)),
            "current_sha256": sha256(current),
            "comparison_method": "same_capture_index_when_available",
            "visual_authenticity": "UNVERIFIED",
        }
        if reference:
            try:
                ref_metrics = read_png(reference)
                cur_metrics = decoded[current]
                if (ref_metrics["width"], ref_metrics["height"]) != (cur_metrics["width"], cur_metrics["height"]):
                    comparison["status"] = "REFERENCE_RESOLUTION_MISMATCH"
                    comparison["reference_frame"] = str(reference)
                    comparison["reference_resolution"] = [ref_metrics["width"], ref_metrics["height"]]
                    comparison["current_resolution"] = [cur_metrics["width"], cur_metrics["height"]]
                else:
                    a, b = ref_metrics["sample_grid_rgb"], cur_metrics["sample_grid_rgb"]
                    absolute = [abs(int(x) - int(y)) for pa, pb in zip(a, b) for x, y in zip(pa, pb)]
                    comparison.update({
                        "reference_frame": str(reference),
                        "reference_sha256": sha256(reference),
                        "mean_absolute_rgb_difference_64x36": round(sum(absolute) / max(1, len(absolute)), 3),
                        "identical_sample_fraction_64x36": round(sum(x == y for x, y in zip(a, b)) / max(1, len(a)), 6),
                        "reference_visual_authenticity": "UNVERIFIED",
                    })
            except Exception as exc:
                comparison["status"] = "REFERENCE_ANALYSIS_FAILED"
                comparison["analysis_error"] = str(exc)
    return records, comparison


def parse_log_metrics(stdout: str, stderr: str) -> dict[str, object]:
    text = stdout + "\n" + stderr
    presents = list(PRESENT_RE.finditer(text))
    latest = presents[-1] if presents else None
    backend_match = re.search(r"Native Apple Metal GNM Command Processor initialized \(([^)]+)\)", text)
    vulkan_match = re.search(r"(?im)^.*(?:Vulkan device|Vulkan renderer|Vulkan backend)[: ]+([^\r\n]+)", text)
    gow3gpu_route_match = re.search(
        r"\[gow3gpu-route\] active PM4 owner=gow3gpu/shadPS4", text, re.I
    )
    candidate_videoout_open = bool(re.search(r"\[gow3gpu-route\] VideoOut handle \d+ opened", text))
    candidate_videoout_registration = bool(re.search(r"\[gow3gpu-route\] registered \d+ display buffers", text))
    candidate_videoout_accepts = re.findall(
        r"\[gow3gpu-route\] candidate Vulkan VideoOut accepted flip (\d+)", text
    )
    candidate_videoout_handoffs = re.findall(
        r"\[gow3gpu-route\] candidate Vulkan VideoOut presenter handoff (\d+)", text
    )
    candidate_device_match = re.search(r"(?m)^GPU: using (.+)$", text)
    swapchain_format_match = re.search(
        r"\[gow3gpu-vk\] swapchain selected surface format=([^\s]+)", text
    )
    heuristic_msl_pipelines = len(re.findall(r"Heuristic MSL substitute compiled", text))
    gcn_selected = bool(re.search(r"GCN translated shader path: enabled", text, re.I))
    gcn_draws = len(re.findall(r"\[gcn-debug\] draw VS=", text))
    gcn_fallback_logs = re.findall(r"\[ps4-gnm-metal\] GCN fallback x(\d+) \(([^)]+)\)", text)
    shader_errors = re.findall(r"(?im)^.*(?:shader|pipeline).*(?:failed|error|unsupported).*$", text)
    unsupported_lines = re.findall(r"(?im)^.*(?:not implemented|unimplemented|unsupported).*$", text)
    unsupported = next((
        line for line in unsupported_lines
        if not re.search(r"\[ps4-gnm-metal\]\s+GPU_CAPTURE:.*replay not implemented", line, re.I)
    ), None)
    if latest:
        groups = latest.groups()
        snapshot = {
            "presentation_number": int(groups[0]),
            "flip_address": groups[1],
            "width": int(groups[2]), "height": int(groups[3]),
            "pm4_commands_decoded": int(groups[4]),
            "draws_intercepted": int(groups[5]), "draws_encoded": int(groups[6]),
            "vertex_buffers_bound": int(groups[7]), "index_buffers_bound": int(groups[8]),
            "texture_bindings": int(groups[9]), "dispatches_encoded": int(groups[10]),
            "pipeline_states_created": int(groups[11]), "render_target_0": groups[12],
        }
    else:
        snapshot = None
    if gow3gpu_route_match:
        active = (
            "gow3gpu/shadPS4 Vulkan PM4 + Vulkan VideoOut"
            if candidate_videoout_handoffs
            else "gow3gpu/shadPS4 Vulkan PM4 (VideoOut handoff not observed)"
        )
    else:
        active = "native-metal" if backend_match else ("vulkan" if vulkan_match else "not-confirmed")
    candidate_shader_frontend_observed = bool(
        gow3gpu_route_match and re.search(r"emit_spirv\.cpp|shader_recompiler", text, re.I)
    )
    return {
        "configured_backend_from_source": "native-metal fallback + opt-in gow3gpu/shadPS4",
        "active_backend": active,
        "metal_device": backend_match.group(1) if backend_match else None,
        "vulkan_device": (
            candidate_device_match.group(1).strip()
            if gow3gpu_route_match and candidate_device_match
            else vulkan_match.group(1).strip() if vulkan_match else None
        ),
        "vulkan_swapchain_format": swapchain_format_match.group(1) if swapchain_format_match else None,
        "gow3gpu_pm4_owner_installed": bool(gow3gpu_route_match),
        "gow3gpu_shader_frontend_observed": candidate_shader_frontend_observed,
        "presentation_backend": (
            "gow3gpu/shadPS4 Vulkan VideoOut" if candidate_videoout_handoffs
            else "native-metal" if backend_match else "not-confirmed"
        ),
        "presentation_log_entries": len(presents),
        "candidate_videoout_open_observed": candidate_videoout_open,
        "candidate_videoout_buffer_registration_observed": candidate_videoout_registration,
        "candidate_videoout_acceptance_log_entries": len(candidate_videoout_accepts),
        "candidate_videoout_presenter_handoff_log_entries": len(candidate_videoout_handoffs),
        "latest_pm4_snapshot": snapshot,
        "heuristic_msl_pipeline_log_entries": heuristic_msl_pipelines,
        "gcn_translation_path": (
            "SHADPS4_FRONTEND_OBSERVED"
            if candidate_shader_frontend_observed
            else "SHADPS4_ROUTE_ACTIVE_FRONTEND_UNCONFIRMED"
            if gow3gpu_route_match
            else "MSL_TRANSLATOR_SELECTED" if gcn_selected else "BUILT_OPT_IN_NOT_SELECTED"
        ),
        "gcn_translated_draw_debug_records": gcn_draws,
        "gcn_fallback_log_records": [
            {"reported_occurrence": int(count), "reason": reason}
            for count, reason in gcn_fallback_logs
        ],
        "shader_pipeline_errors": shader_errors[:20],
        "first_unsupported_operation": unsupported.strip() if unsupported else None,
        "gpu_synchronization_state": "NOT_INSTRUMENTED",
    }


def milestone_summary(stdout: str, stderr: str, gpu: dict[str, object]) -> dict[str, bool]:
    text = stdout + "\n" + stderr
    return {
        "runtime_started": bool(re.search(r"Initializing runtime|Loading guest memory image", text, re.I)),
        "guest_memory_loaded": bool(re.search(r"guest memory at host|Loading guest memory image", text, re.I)),
        "entrypoint_started": bool(re.search(r"Executing _start|via ARM64 assembly", text, re.I)),
        "metal_backend_initialized": bool(gpu["metal_device"]),
        "metal_videoout_initialized": bool(gpu["metal_device"]),
        "gow3gpu_pm4_owner_installed": bool(gpu["gow3gpu_pm4_owner_installed"]),
        "gow3gpu_shader_frontend_observed": bool(gpu["gow3gpu_shader_frontend_observed"]),
        "candidate_videoout_open_observed": bool(gpu["candidate_videoout_open_observed"]),
        "candidate_videoout_buffer_registration_observed": bool(gpu["candidate_videoout_buffer_registration_observed"]),
        "candidate_videoout_presentation_observed": bool(
            gpu["candidate_videoout_presenter_handoff_log_entries"]
        ),
        "at_least_one_logged_presentation": bool(
            gpu["presentation_log_entries"] or gpu["candidate_videoout_presenter_handoff_log_entries"]
        ),
        "game_completion_reached": "Execution complete." in text,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one bounded GOW3 smoke iteration through Launch_GOW3.command.",
        epilog="Exit codes: 0 bounded run captured a frame; 2 build failure; 3 assets missing; 4 non-ARM64 binary; 5 start/stall; 6 crash; 7 no screenshot.",
    )
    parser.add_argument("--mode", choices=("smoke",), default="smoke",
                        help="Implemented lab mode: bounded startup and internal framebuffer capture.")
    parser.add_argument("--runtime-seconds", type=float, default=float(os.environ.get("GOW3_LAB_RUNTIME_SECONDS", "60")))
    parser.add_argument("--build-timeout-seconds", type=float, default=600)
    parser.add_argument("--build-no-progress-seconds", type=float, default=180)
    parser.add_argument("--no-progress-seconds", type=float, default=25)
    parser.add_argument("--screenshot-every", type=int, default=60)
    parser.add_argument("--frame-dump-every", type=int, default=240,
                        help="candidate presenter frames between raw frame dumps")
    parser.add_argument("--frame-dump-max", type=int, default=4,
                        help="maximum candidate raw frame dumps per run")
    parser.add_argument("--pass-capture-every", type=int, default=240,
                        help="candidate frames between render-pass traces")
    parser.add_argument("--pass-capture-max", type=int, default=3,
                        help="maximum candidate render-pass traces per run")
    parser.add_argument("--max-log-mb", type=int, default=32)
    parser.add_argument("--max-capture-mb", type=int, default=512)
    parser.add_argument("--max-rss-mb", type=int, default=8192)
    parser.add_argument("--skip-build", action="store_true", help="Use the existing binary; the default always runs Ninja first.")
    parser.add_argument("--build-only", action="store_true", help="Build and report architecture without starting the game.")
    args = parser.parse_args()
    # GOW3_LAB_MAX_SECONDS raises the bound for driven sessions (menus, loading a save).
    max_seconds = float(os.environ.get("GOW3_LAB_MAX_SECONDS", "300"))
    if not 1 <= args.runtime_seconds <= max_seconds:
        parser.error(f"--runtime-seconds must be between 1 and {max_seconds:g} for the bounded smoke mode")
    if not 1 <= args.max_log_mb <= 128:
        parser.error("--max-log-mb must be between 1 and 128")
    if not 128 <= args.max_capture_mb <= 4096:
        parser.error("--max-capture-mb must be between 128 and 4096")
    if not 512 <= args.max_rss_mb <= 16384:
        parser.error("--max-rss-mb must be between 512 and 16384")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d_%H%M%S_%f")
    run_id = f"run_{stamp}"
    run_dir = RUNS_DIR / run_id
    (run_dir / "frames").mkdir(parents=True, exist_ok=False)
    (run_dir / "gpu-captures").mkdir()
    (run_dir / "candidate-dump").mkdir()
    (run_dir / "failures").mkdir()
    baseline = {
        "mode": args.mode,
        "run_id": run_id,
        "runner_pid": os.getpid(),
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "repo_path": str(BASE_DIR),
        "git_revision": git_revision(),
        "git_metadata_available": (BASE_DIR / ".git").exists(),
        "source_sha256": source_fingerprints(),
        "host_architecture": platform.machine(),
        "host_platform": platform.platform(),
        "renderer_requested": os.environ.get("PS4_GOW3GPU_BACKEND", "native-metal"),
        "renderer_library": os.environ.get("PS4_GOW3GPU_LIBRARY"),
        "renderer_configured": (
            "gow3gpu/shadPS4 candidate (opt-in request; runtime log must confirm initialization)"
            if os.environ.get("PS4_GOW3GPU_BACKEND") == "gow3gpu-vulkan"
            else "native-metal (direct GNM/VideoOut path verified from source)"
        ),
        "selected_asset_directory": os.environ.get("GOW3_DIR", str(DEFAULT_ASSETS)),
        "stages": {},
    }
    write_json(run_dir / "run.json", baseline)
    print(f"RUN: {run_id}")
    print(f"REPO: {BASE_DIR}")
    print(f"ARTIFACTS: {run_dir}")
    print(f"RUNNER_PID: {os.getpid()} (send SIGINT to stop this run safely)")
    if baseline["git_revision"] is None:
        print("REVISION: unavailable (directory has no Git metadata; source hashes recorded)")

    build_result: dict[str, object]
    if args.skip_build:
        build_result = {"status": "SKIPPED_BY_REQUEST", "command": None}
        (run_dir / "build.log").write_text("Build skipped by --skip-build.\n", encoding="utf-8")
    else:
        build_env = os.environ.copy()
        build_result = run_bounded(
            ["ninja", "-j4", "ps4_app"], BASE_DIR, build_env,
            run_dir / "build.log", None, args.build_timeout_seconds,
            args.build_no_progress_seconds, args.max_log_mb * 1024 * 1024,
            args.max_rss_mb,
        )
    baseline["stages"]["build"] = build_result
    build_ok = build_result.get("status") in ("EXITED", "SKIPPED_BY_REQUEST") and build_result.get("exit_code", 0) in (0, None)
    if not build_ok:
        excerpt = first_error_excerpt(run_dir / "build.log")
        if excerpt:
            (run_dir / "failures" / "build-error.txt").write_text(excerpt + "\n", encoding="utf-8")
        baseline["session_verdict"] = "FAIL"
        baseline["first_blocker"] = excerpt or str(build_result)
        write_json(run_dir / "run.json", baseline)
        write_json(run_dir / "verdict.json", {"session_verdict": "FAIL", "build": build_result, "first_blocker": baseline["first_blocker"]})
        print("BUILD: FAIL")
        print(f"FIRST ERROR: {excerpt or build_result}")
        return 2

    binary = BASE_DIR / "ps4_app"
    file_result = subprocess.run(["file", str(binary)], text=True, capture_output=True)
    lipo_result = subprocess.run(["lipo", "-info", str(binary)], text=True, capture_output=True)
    arch_text = file_result.stdout.strip() + "\n" + lipo_result.stdout.strip()
    binary_arch = "arm64" if re.search(r"\barm64\b", arch_text, re.I) else "unknown"
    baseline["binary"] = {"path": str(binary), "architecture": binary_arch, "file_output": file_result.stdout.strip(), "lipo_output": lipo_result.stdout.strip(), "sha256": sha256(binary) if binary.exists() else None}
    print(f"BUILD: PASS ({build_result.get('duration_seconds', 0)}s)")
    print(f"ARCH: {binary_arch}")
    if binary_arch != "arm64":
        baseline["session_verdict"] = "FAIL"
        baseline["first_blocker"] = "ps4_app is not an ARM64 Mach-O executable"
        write_json(run_dir / "run.json", baseline)
        write_json(run_dir / "verdict.json", {"session_verdict": "FAIL", "first_blocker": baseline["first_blocker"]})
        return 4
    if args.build_only:
        baseline["session_verdict"] = "PARTIAL"
        baseline["runtime"] = {"status": "NOT_RUN_BUILD_ONLY"}
        write_json(run_dir / "run.json", baseline)
        write_json(run_dir / "verdict.json", {"session_verdict": "PARTIAL", "build": "PASS", "runtime": "NOT_RUN_BUILD_ONLY"})
        print("RUNTIME: SKIPPED (--build-only)")
        return 0

    asset_dir = Path(baseline["selected_asset_directory"]).expanduser()
    if not asset_dir.is_dir():
        baseline["stages"]["runtime"] = {"status": "BLOCKED_ASSETS_MISSING", "path": str(asset_dir)}
        baseline["session_verdict"] = "BLOCKED"
        baseline["first_blocker"] = f"Game asset directory is unavailable: {asset_dir}"
        write_json(run_dir / "run.json", baseline)
        write_json(run_dir / "verdict.json", {"session_verdict": "BLOCKED", "first_blocker": baseline["first_blocker"]})
        print(f"ASSETS: MISSING ({asset_dir})")
        return 3
    baseline["assets_accessible"] = True

    # Protect an interactive/manual launch from an overlapping GPU-heavy test.
    # `ps -axo pid=,comm=` reports the executable name without matching this
    # Python harness or a shell command that merely mentions ps4_app.
    ps_result = subprocess.run(
        ["ps", "-axo", "pid=,comm="], text=True, capture_output=True, check=False,
    )
    active_app_pids = []
    if ps_result.returncode == 0:
        for line in ps_result.stdout.splitlines():
            fields = line.strip().split(None, 1)
            if len(fields) == 2 and Path(fields[1].strip()).name == "ps4_app":
                try:
                    active_app_pids.append(int(fields[0]))
                except ValueError:
                    continue
    if active_app_pids:
        baseline["stages"]["runtime"] = {
            "status": "BLOCKED_EXISTING_APP_PROCESS",
            "pids": active_app_pids,
            "note": "An existing ps4_app was left untouched; no second runtime was started.",
        }
        baseline["session_verdict"] = "BLOCKED"
        baseline["first_blocker"] = f"Existing ps4_app process(es) {active_app_pids}; close the current game before a lab runtime."
        write_json(run_dir / "run.json", baseline)
        write_json(run_dir / "verdict.json", {
            "session_verdict": "BLOCKED",
            "build": "PASS",
            "runtime": baseline["stages"]["runtime"],
            "first_blocker": baseline["first_blocker"],
        })
        print(f"RUNTIME: BLOCKED (existing ps4_app PID(s): {', '.join(map(str, active_app_pids))})")
        return 5

    runtime_env = os.environ.copy()
    runtime_env["GOW3_DIR"] = str(asset_dir)
    runtime_env.setdefault("PS4_APP_DIR", str(asset_dir))
    runtime_env.setdefault("PS4_STALL_WATCHDOG", "3000")
    runtime_env.setdefault("PS4_TRACE_HOST_QUIT", "1")
    runtime_env["PS4_SCREENSHOT_DIR"] = str(run_dir / "frames")
    runtime_env["PS4_SCREENSHOT_EVERY"] = str(max(1, args.screenshot_every))
    runtime_env["PS4_GPU_CAPTURE_DIR"] = str(run_dir / "gpu-captures")
    # Lab runs never touch the player's saves (~/Library/Application Support/PS4Recomp/saves):
    # the auto-pressed New Game writes AUTOSAVE, so every run gets its own empty save folder.
    # GOW3_LAB_SAVEDATA_DIR picks another throwaway folder (e.g. a copy of a save to load).
    lab_saves = Path(os.environ.get("GOW3_LAB_SAVEDATA_DIR", str(run_dir / "savedata"))).expanduser()
    user_saves = (Path.home() / "Library/Application Support/PS4Recomp/saves").resolve()
    if lab_saves.resolve() == user_saves or user_saves in lab_saves.resolve().parents:
        print(f"RUNTIME: BLOCKED (save folder {lab_saves} is the player's save folder)")
        return 5
    lab_saves.mkdir(parents=True, exist_ok=True)
    runtime_env["PS4_SAVEDATA_DIR"] = str(lab_saves)
    runtime_env.pop("PS4RECOMP_DATA_DIR", None)
    candidate_backend =runtime_env.get("PS4_GOW3GPU_BACKEND") == "gow3gpu-vulkan"
    if candidate_backend:
        candidate_user_dir = Path(runtime_env.get(
            "PS4_GOW3GPU_USER_DIR",
            str(Path.home() / "Library/Application Support/GOW3Lab/candidate-user"),
        )).expanduser()
        runtime_env["PS4_GOW3GPU_USER_DIR"] = str(candidate_user_dir)
        runtime_env["GOW3_GPU_USER_DIR"] = str(candidate_user_dir)
        runtime_env["GOW3_CAPTURE_DIR"] = str(run_dir / "gpu-captures")
        capture_trigger = run_dir / "candidate-pass-trace.trigger"
        capture_trigger.touch()
        runtime_env["GOW3_CAPTURE_TRIGGER"] = str(capture_trigger)
        dump_trigger = run_dir / "candidate-frame-dump.trigger"
        dump_trigger.touch()
        runtime_env["GOW3_DUMP_DIR"] = str(run_dir / "candidate-dump")
        runtime_env["GOW3_FRAME_DUMP_TRIGGER"] = str(dump_trigger)
        runtime_env["GOW3_FRAME_DUMP_EVERY"] = str(max(1, args.frame_dump_every))
        runtime_env["GOW3_FRAME_DUMP_MAX"] = str(max(0, args.frame_dump_max))
        runtime_env["GOW3_CAPTURE_EVERY"] = str(max(1, args.pass_capture_every))
        runtime_env["GOW3_CAPTURE_MAX"] = str(max(0, args.pass_capture_max))
    runtime_env["PS4_AUDIO_LOG"] = "1"
    runtime_env["PS4_AJM_LOG"] = "1"
    audio_dump_path = run_dir / "audio-mix.wav"
    runtime_env["PS4_AUDIO_DUMP"] = str(audio_dump_path)
    capture_host_resources(run_dir / "host-resources.txt", "before-runtime")
    runtime_result = run_bounded(
        ["/bin/bash", str(BASE_DIR / "Launch_GOW3.command")], BASE_DIR,
        runtime_env, run_dir / "stdout.log", run_dir / "stderr.log",
        args.runtime_seconds, args.no_progress_seconds,
        args.max_log_mb * 1024 * 1024, args.max_rss_mb,
        progress_files=run_dir / "frames", capture_dirs=[run_dir],
        max_capture_bytes=args.max_capture_mb * 1024 * 1024,
    )
    capture_host_resources(run_dir / "host-resources.txt", "after-runtime")
    baseline["stages"]["runtime"] = runtime_result

    stdout = (run_dir / "stdout.log").read_text(errors="replace")
    stderr = (run_dir / "stderr.log").read_text(errors="replace")
    full_text = stdout + "\n" + stderr
    candidate_frame_dumps = convert_candidate_frame_dumps(run_dir, full_text)
    gpu = parse_log_metrics(stdout, stderr)
    milestones = milestone_summary(stdout, stderr, gpu)
    frames, comparison = analyze_frames(frame_files(run_dir / "frames"), run_dir)
    frame_classifications = Counter(
        record.get("frame_assessment", "ANALYSIS_FAILED") for record in frames
    )
    latest_frame = next(
        (record for record in reversed(frames) if "frame_assessment" in record), None
    )
    capture_files = sorted(
        path for directory in (run_dir / "gpu-captures", run_dir / "candidate-dump")
        for path in directory.iterdir() if path.is_file()
    )
    capture_has_trace = any(path.suffix == ".txt" for path in capture_files)
    capture_has_raw = any(path.suffix == ".raw" for path in capture_files)
    capture_has_json = any(path.suffix == ".json" for path in capture_files)
    capture_status = (
        "CAPTURED_RENDER_PASS_TRACE_AND_RAW_FRAME" if capture_has_trace and capture_has_raw
        else "CAPTURED_RENDER_PASS_TRACE" if capture_has_trace
        else "CAPTURED_RAW_FRAME" if capture_has_raw
        else "CAPTURED_PARTIAL_DRAW_SNAPSHOT" if capture_has_json
        else "NOT_CAPTURED"
    )
    capture_manifest = {
        "status": capture_status,
        "replay_status": "NOT_IMPLEMENTED",
        "files": [
            {"file": str(path.relative_to(run_dir)), "bytes": path.stat().st_size, "sha256": sha256(path)}
            for path in capture_files
        ],
    }
    write_json(run_dir / "gpu-captures" / "manifest.json", capture_manifest)
    gpu["draw_capture_status"] = capture_manifest["status"]
    gpu["draw_capture_file_count"] = len(capture_files)
    gpu["candidate_frame_dump_records"] = candidate_frame_dumps
    gpu["candidate_frame_dump_png_count"] = sum(
        record.get("status") == "CONVERTED" for record in candidate_frame_dumps
    )
    gpu["candidate_output_png_count"] = sum(
        record.get("status") == "CONVERTED" and
        str(record.get("raw_file", "")).startswith("candidate-dump/output_")
        for record in candidate_frame_dumps
    )
    write_json(run_dir / "gpu-summary.json", gpu)
    write_json(run_dir / "shader-summary.json", {
        "status": gpu["gcn_translation_path"],
        "heuristic_msl_pipeline_log_entries": gpu["heuristic_msl_pipeline_log_entries"],
        "gcn_translated_draw_debug_records": gpu["gcn_translated_draw_debug_records"],
        "gcn_fallback_log_records": gpu["gcn_fallback_log_records"],
        "translation_correctness": "UNVERIFIED",
        "backend": gpu["active_backend"],
        "shader_errors": gpu["shader_pipeline_errors"],
    })
    thread_lines = [line for line in full_text.splitlines() if re.search(r"thread|stall|watchdog|deadlock", line, re.I)]
    write_json(run_dir / "thread-summary.json", {
        "guest_thread_state": "NOT_INSTRUMENTED",
        "watchdog_or_thread_log_lines": thread_lines[:100],
        "thread_log_line_count": len(thread_lines),
    })
    audio_lines = [line for line in full_text.splitlines() if re.search(r"audio|coreaudio|atrac|audiodec", line, re.I)]
    audio_metrics = inspect_float_wav(audio_dump_path)
    audioout_metrics = parse_audioout_log(full_text)
    video_lines = [line for line in full_text.splitlines() if re.search(r"videodec|video decoder|decoded frame", line, re.I)]
    write_json(run_dir / "audio-video-summary.json", {
        "audio_status": audio_metrics["status"],
        "audible_output": "UNVERIFIED_PHYSICALLY",
        "audio_mode": {
            "ps4_audio_mute": runtime_env.get("PS4_AUDIO_MUTE", "0"),
            "ps4_audio_null": runtime_env.get("PS4_AUDIO_NULL", "0"),
        },
        "audio_dump": audio_metrics,
        "audioout_runtime_metrics": audioout_metrics,
        "ajm_decode_metrics": parse_ajm_decode_log(full_text),
        "audio_log_lines": audio_lines[:60],
        "ajm_decode_log_lines": [line for line in full_text.splitlines() if "[AJM]" in line][-60:],
        "video_status": "NOT_VERIFIED",
        "video_decode_log_lines": video_lines[:60],
    })
    write_json(run_dir / "frames-summary.json", {
        "frame_count": len(frames),
        "classification_counts": dict(frame_classifications),
        "latest_frame": latest_frame,
        "visual_authenticity": "UNVERIFIED",
        "frames": frames,
    })
    write_json(run_dir / "comparison.json", comparison)
    baseline["runtime"] = runtime_result
    baseline["active_graphics_backend"] = gpu["active_backend"]
    baseline["startup_milestones"] = milestones
    baseline["captured_frame_count"] = len(frames)
    baseline["gpu_draw_capture"] = capture_manifest
    baseline["candidate_frame_dump"] = candidate_frame_dumps
    baseline["frame_analysis"] = {
        "classification_counts": dict(frame_classifications),
        "latest_frame": latest_frame,
        "visual_authenticity": "UNVERIFIED",
    }
    baseline["first_unsupported_operation"] = gpu["first_unsupported_operation"]
    baseline["warnings_and_errors"] = [line for line in full_text.splitlines() if re.search(r"warning|error|fatal|unimplemented|unsupported", line, re.I)][:80]

    if runtime_result["status"] == "INTERRUPTED":
        verdict, exit_code = "PARTIAL", 130
        blocker = "Run was stopped by the operator; only the child process group started by this runner was terminated."
    elif runtime_result["status"] == "CRASHED":
        verdict, exit_code = "FAIL", 6
        blocker = "Application exited with a nonzero status before the bounded run ended."
    elif runtime_result["status"] == "START_FAILED":
        verdict, exit_code = "FAIL", 5
        blocker = str(runtime_result.get("error", "Application failed to start."))
    elif runtime_result["termination_reason"] == "no_progress":
        verdict, exit_code = "FAIL", 5
        blocker = "Application made no log or screenshot progress before the watchdog deadline."
    elif runtime_result["termination_reason"] in ("memory_limit", "log_size_limit", "capture_size_limit"):
        verdict, exit_code = "FAIL", 5
        blocker = f"Bounded run stopped at {runtime_result['termination_reason']} ({runtime_result.get('max_capture_bytes', 0)} captured bytes)."
    elif not frames:
        verdict, exit_code = "FAIL", 7
        blocker = "No screenshot was saved during the bounded runtime."
    else:
        verdict, exit_code = "PARTIAL", 0
        if gpu["gow3gpu_pm4_owner_installed"]:
            if not gpu["candidate_videoout_open_observed"]:
                blocker = (
                    "The shadPS4 Vulkan PM4 route initialized, but the guest did not open the candidate "
                    "VideoOut presenter in this bounded run. Candidate presentation and post-splash "
                    "graphics remain unverified."
                )
            elif not gpu["candidate_videoout_presenter_handoff_log_entries"]:
                blocker = (
                    "The guest opened the candidate VideoOut path, but no presenter handoff was observed. "
                    "Inspect VideoOut registration/flip logs and the first Vulkan capture for the point "
                    "where guest presentation stops."
                )
            elif not gpu["candidate_output_png_count"]:
                blocker = (
                    "The candidate Vulkan presenter accepted flips, but the one-shot internal frame dump "
                    "did not produce a convertible output image. PM4/shader processing is observed; "
                    "visual correctness remains unverified."
                )
            else:
                blocker = (
                    "The candidate Vulkan path produced internally captured frames, but visual authenticity "
                    "and post-splash game correctness remain unverified. Inspect the exported PNGs and "
                    "render-pass trace before changing GPU state."
                )
        elif gpu["gcn_translation_path"] == "BUILT_OPT_IN_NOT_SELECTED":
            blocker = (
                "The active Metal path used heuristic MSL substitutes; the vendored GCN-to-SPIR-V/MSL path "
                "is opt-in and was not selected. Correct geometry remains unverified."
            )
        elif frame_classifications.get("MALFORMED_POLYGON_CANDIDATE", 0):
            blocker = "Captured frames include a malformed-polygon candidate; vertex fetch/transform, viewport/scissor, and primitive assembly remain unverified."
        else:
            blocker = "Frame authenticity and correct geometry are unverified; the captured output requires renderer/GPU-state diagnosis."
    baseline["session_verdict"] = verdict
    baseline["first_blocker"] = blocker
    baseline["frame_authenticity"] = "UNVERIFIED"
    write_json(run_dir / "run.json", baseline)
    write_json(run_dir / "verdict.json", {
        "session_verdict": verdict,
        "build": "PASS",
        "binary_architecture": binary_arch,
        "runtime_status": runtime_result["status"],
        "runtime_termination_reason": runtime_result["termination_reason"],
        "active_graphics_backend": gpu["active_backend"],
        "gpu_draw_capture": capture_manifest,
        "startup_milestones": milestones,
        "captured_frames": len(frames),
        "frame_classification_counts": dict(frame_classifications),
        "latest_frame": latest_frame,
        "frame_authenticity": "UNVERIFIED",
        "first_blocker": blocker,
        "comparison": comparison,
    })
    report = [
        f"# GOW3 run {run_id}", "", f"- Session verdict: **{verdict}**",
        f"- Build: **PASS** ({binary_arch})", f"- Runtime: **{runtime_result['status']}** ({runtime_result['termination_reason']})",
        f"- Active graphics backend: **{gpu['active_backend']}**" + (
            f"; swapchain format `{gpu['vulkan_swapchain_format']}`"
            if gpu["vulkan_swapchain_format"] else ""
        ),
        f"- Frames captured: **{len(frames)}**; authenticity: **UNVERIFIED**",
        f"- Frame classifications: `{json.dumps(dict(frame_classifications), sort_keys=True)}`",
        f"- Latest frame assessment: **{latest_frame['frame_assessment']}**" if latest_frame else "- Latest frame assessment: unavailable",
        f"- Image comparison: **{comparison.get('status')}**" + (
            f"; same-index MAE {comparison['mean_absolute_rgb_difference_64x36']} / 255"
            if comparison.get("mean_absolute_rgb_difference_64x36") is not None else ""
        ),
        f"- GPU state capture: **{capture_manifest['status']}** ({len(capture_files)} files); replay: NOT_IMPLEMENTED",
        f"- Candidate Vulkan presenter handoff log entries: **{gpu['candidate_videoout_presenter_handoff_log_entries']}**",
        f"- Candidate frame PNGs: **{gpu['candidate_frame_dump_png_count']}** (final output {gpu['candidate_output_png_count']})",
        f"- First blocker: {blocker}",
        f"- Artifacts: `{run_dir}`", "",
        "Counters and color statistics describe observed activity only. They do not establish correct rendering.",
    ]
    (run_dir / "summary.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"RUNTIME: {runtime_result['status']} ({runtime_result['termination_reason']}, {runtime_result['duration_seconds']}s)")
    print(f"BACKEND: {gpu['active_backend']}" + (f" ({gpu['metal_device']})" if gpu["metal_device"] else ""))
    print(f"PRESENTS: Metal logs {gpu['presentation_log_entries']} | candidate handoff logs {gpu['candidate_videoout_presenter_handoff_log_entries']} | FRAMES: {len(frames)} | AUTHENTICITY: UNVERIFIED")
    print(f"FRAME TYPES: {dict(frame_classifications)}")
    print(f"GPU CAPTURE: {capture_manifest['status']} ({len(capture_files)} files), replay NOT_IMPLEMENTED")
    print(f"VERDICT: {verdict}")
    print(f"FIRST BLOCKER: {blocker}")
    print(f"REPORT: {run_dir / 'summary.md'}")
    return exit_code


def capture_host_resources(path: Path, phase: str) -> None:
    """Record read-only thermal and memory-pressure data when macOS exposes it."""
    timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
    sections = [f"[{phase}] {timestamp}"]
    for label, command in (("thermal", ["pmset", "-g", "therm"]),
                           ("memory_pressure", ["memory_pressure", "-Q"])):
        try:
            result = subprocess.run(command, text=True, capture_output=True, timeout=3, check=False)
            sections.append(f"{label}: exit={result.returncode}")
            sections.append((result.stdout + result.stderr).strip() or "unavailable")
        except (OSError, subprocess.TimeoutExpired) as exc:
            sections.append(f"{label}: unavailable ({type(exc).__name__})")
    with path.open("a", encoding="utf-8") as stream:
        stream.write("\n".join(sections) + "\n")


if __name__ == "__main__":
    sys.exit(main())
