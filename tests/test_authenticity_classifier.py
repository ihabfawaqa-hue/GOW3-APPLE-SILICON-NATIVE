#!/usr/bin/env python3
"""Coarse framebuffer labels never authenticate a game image."""

import math
from pathlib import Path
import random
import sys

LAB_DIR = Path(__file__).resolve().parents[1] / "scripts" / "lab"
sys.path.insert(0, str(LAB_DIR))
from run_iteration import assess_frame  # noqa: E402


def classify_pixels(pixels_bgra, width, height):
    total = width * height
    histogram = {}
    black = left_black = right_black = left_count = right_count = 0
    lum_sum = lum_sq_sum = 0.0
    for index, pixel in enumerate(pixels_bgra):
        x = index % width
        b, g, r = pixel & 0xFF, (pixel >> 8) & 0xFF, (pixel >> 16) & 0xFF
        is_black = r <= 8 and g <= 8 and b <= 8
        black += int(is_black)
        if x < width // 2:
            left_black += int(is_black)
            left_count += 1
        else:
            right_black += int(is_black)
            right_count += 1
        key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
        histogram[key] = histogram.get(key, 0) + 1
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        lum_sum += lum
        lum_sq_sum += lum * lum

    mean = lum_sum / total
    stddev = math.sqrt(max(0.0, lum_sq_sum / total - mean * mean))
    assessment = assess_frame(
        total, black / total, len(histogram), max(histogram.values()) / total,
        stddev, left_black / max(1, left_count), right_black / max(1, right_count),
    )
    return {"frame_assessment": assessment, "visual_authenticity": "UNVERIFIED"}


def test_classifier():
    black = [0x00000000] * 10000
    result = classify_pixels(black, 100, 100)
    assert result == {"frame_assessment": "NO_FRAME_OR_BLACK", "visual_authenticity": "UNVERIFIED"}

    flat_white = [0x00FFFFFF] * 10000
    result = classify_pixels(flat_white, 100, 100)
    assert result["frame_assessment"] == "FLAT_CLEAR_OR_FALLBACK"
    assert result["visual_authenticity"] == "UNVERIFIED"

    # Approximate the real capture's few green/purple bins and black left half.
    polygon = []
    for y in range(100):
        for x in range(100):
            if x < 52:
                polygon.append(0x00000000)
            elif (x + y) % 5 < 2:
                polygon.append(0x00A8F020)
            else:
                polygon.append(0x00A030C0)
    result = classify_pixels(polygon, 100, 100)
    assert result["frame_assessment"] == "MALFORMED_POLYGON_CANDIDATE"
    assert result["visual_authenticity"] == "UNVERIFIED"

    rng = random.Random(42)
    textured = []
    for y in range(100):
        for x in range(100):
            r = rng.randrange(256)
            g = rng.randrange(256)
            b = rng.randrange(256)
            textured.append((r << 16) | (g << 8) | b)
    result = classify_pixels(textured, 100, 100)
    assert result["frame_assessment"] == "NONTRIVIAL_RENDER_UNVERIFIED"
    assert result["visual_authenticity"] == "UNVERIFIED"


if __name__ == "__main__":
    test_classifier()
    print("PASS: black, flat, polygon-candidate, and high-variation frames remain UNVERIFIED")
