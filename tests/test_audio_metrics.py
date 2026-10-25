#!/usr/bin/env python3
from __future__ import annotations

"""Validate host-mix WAV and AudioOut log analysis without using game data."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts" / "lab"))
from audio_metrics import inspect_float_wav, parse_ajm_decode_log, parse_audioout_log


def write_float_wav(path: Path, samples: list[float], *, declared_size: int | None = None) -> None:
    pcm = struct.pack(f"<{len(samples)}f", *samples)
    size = len(pcm) if declared_size is None else declared_size
    header = (
        b"RIFF" + struct.pack("<I", 36 + size) + b"WAVE"
        + b"fmt " + struct.pack("<IHHIIHH", 16, 3, 2, 48000, 384000, 8, 32)
        + b"data" + struct.pack("<I", size)
    )
    path.write_bytes(header + pcm)


class AudioMetricsTests(unittest.TestCase):
    def test_nonzero_pcm_reports_host_mix_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "audio-mix.wav"
            write_float_wav(path, [0.5, -0.5, 0.25, -0.25])
            metrics = inspect_float_wav(path)
        self.assertEqual(metrics["status"], "NONZERO_PCM_AT_HOST_MIX")
        self.assertEqual(metrics["frames"], 2)
        self.assertEqual(metrics["nonzero_samples"], 4)
        self.assertEqual(metrics["rms"], round((0.625 / 4) ** 0.5, 8))
        self.assertEqual(metrics["peak"], 0.5)
        self.assertEqual(metrics["audible_output"], "UNVERIFIED_PHYSICALLY")

    def test_silence_and_header_only_are_distinguished(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "audio-mix.wav"
            write_float_wav(path, [0.0, 0.0])
            silence = inspect_float_wav(path)
            path.write_bytes(path.read_bytes()[:44])
            empty = inspect_float_wav(path)
        self.assertEqual(silence["status"], "SILENCE_AT_HOST_MIX")
        self.assertEqual(silence["rms"], 0.0)
        self.assertEqual(empty["status"], "EMPTY_DUMP")

    def test_in_progress_dump_uses_bytes_written_after_stale_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "audio-mix.wav"
            write_float_wav(path, [0.1, -0.1], declared_size=0)
            metrics = inspect_float_wav(path)
        self.assertEqual(metrics["frames"], 1)
        self.assertEqual(metrics["status"], "NONZERO_PCM_AT_HOST_MIX")

    def test_audioout_logs_show_port_and_device_progress_separately(self):
        logs = "\n".join([
            "[AudioOut] port 1 type=0 fmt=7 ch=8 len=256: outputs=10 nonzero_bufs=8 "
            "in_rms=0.12500 in_peak=0.50000 vol=[1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00] "
            "fill=256/1024 underruns=2",
            "[AudioOut] host: running=1 render_calls=20 render_frames=9600 "
            "mix_rms=0.06250 mix_peak=0.25000 dump_frames=9600",
        ])
        metrics = parse_audioout_log(logs)
        self.assertEqual(metrics["latest_port_observations"][0]["nonzero_buffers"], 8)
        self.assertEqual(metrics["latest_port_observations"][0]["underruns"], 2)
        self.assertTrue(metrics["latest_host_observation"]["running"])
        self.assertEqual(metrics["latest_host_observation"]["render_frames"], 9600)

    def test_audioout_pacing_logs_report_cadence_and_queue_steering(self):
        logs = (
            "[AudioOut] pacing port 1: intervals=376 avg_ms=5.333 max_ms=7.200 "
            "planned_wait_avg_ms=4.500 queue_min=768 adjust_us=0.0 resets=1"
        )
        pacing = parse_audioout_log(logs)["latest_pacing_observations"][0]
        self.assertEqual(pacing["intervals"], 376)
        self.assertEqual(pacing["average_interval_ms"], 5.333)
        self.assertEqual(pacing["minimum_queue_frames"], 768)
        self.assertEqual(pacing["clock_resets"], 1)

    def test_ajm_decode_metrics_separate_decoder_output_from_host_audio(self):
        line = (
            "[AJM] decode: inst=16385 codec=1 fmt=2 ch=2 run=2 input_bytes=64 "
            "input_used=64 output_bytes=8192 frames=4 result=0x0 internal=0 "
            "pcm_nonzero=4000 pcm_rms=0.125000 pcm_peak=0.500000 nonfinite=0"
        )
        metrics = parse_ajm_decode_log(line)
        self.assertEqual(metrics["decoded_jobs_with_pcm_bytes"], 1)
        self.assertEqual(metrics["decoded_jobs_with_nonzero_pcm"], 1)
        self.assertEqual(metrics["latest_decode_record"]["frames"], 4)
        self.assertEqual(metrics["latest_decode_record"]["pcm_peak"], 0.5)

    def test_ajm_not_enough_room_is_not_misreported_as_codec_error(self):
        line = (
            "[AJM] decode: inst=16385 codec=1 fmt=2 ch=2 run=2 input_bytes=64 "
            "input_used=64 output_bytes=8192 frames=4 result=0x10 internal=0 "
            "pcm_nonzero=4000 pcm_rms=0.125000 pcm_peak=0.500000 nonfinite=0"
        )
        metrics = parse_ajm_decode_log(line)
        self.assertEqual(metrics["decode_jobs_with_any_nonzero_result"], 1)
        self.assertEqual(metrics["decode_jobs_with_error_result"], 0)
        self.assertEqual(metrics["decode_jobs_with_not_enough_room_result"], 1)


if __name__ == "__main__":
    unittest.main()
