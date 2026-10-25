"""Small, dependency-free analyzers for AudioOut's float32 stereo WAV dump."""

from __future__ import annotations

import math
import re
import struct
from pathlib import Path


def inspect_float_wav(path: Path) -> dict[str, object]:
    """Inspect an in-progress or complete AudioOut float32 stereo WAV safely."""
    if not path.is_file():
        return {"status": "NO_DUMP_FILE", "path": str(path)}

    file_size = path.stat().st_size
    if file_size < 44:
        return {"status": "INCOMPLETE_WAV_HEADER", "path": str(path), "file_bytes": file_size}

    try:
        with path.open("rb") as stream:
            header = stream.read(12)
            if len(header) != 12 or header[:4] != b"RIFF" or header[8:12] != b"WAVE":
                return {"status": "INVALID_RIFF_WAV", "path": str(path), "file_bytes": file_size}

            pos = 12
            fmt: tuple[int, int, int, int, int] | None = None
            data_chunks: list[tuple[int, int]] = []
            while pos + 8 <= file_size:
                stream.seek(pos)
                chunk_header = stream.read(8)
                if len(chunk_header) != 8:
                    break
                chunk_id, declared_size = struct.unpack("<4sI", chunk_header)
                body = pos + 8
                available = max(0, file_size - body)
                if chunk_id == b"fmt ":
                    stream.seek(body)
                    chunk = stream.read(min(declared_size, 40))
                    if len(chunk) < 16:
                        return {"status": "INVALID_FORMAT_CHUNK", "path": str(path), "file_bytes": file_size}
                    tag, channels, rate, _byte_rate, block_align, bits = struct.unpack_from("<HHIIHH", chunk)
                    fmt = (tag, channels, rate, block_align, bits)
                elif chunk_id == b"data":
                    # AudioOut updates the WAV length about once per second. While
                    # running, actual file length may be newer than the RIFF data size.
                    data_chunks.append((body, available))
                    break
                pos = body + declared_size + (declared_size & 1)

            if fmt is None or not data_chunks:
                return {"status": "MISSING_FMT_OR_DATA", "path": str(path), "file_bytes": file_size}
            tag, channels, rate, block_align, bits = fmt
            if (tag, channels, block_align, bits) != (3, 2, 8, 32):
                return {
                    "status": "UNSUPPORTED_WAV_FORMAT", "path": str(path), "file_bytes": file_size,
                    "format_tag": tag, "channels": channels, "sample_rate_hz": rate,
                    "block_align": block_align, "bits_per_sample": bits,
                }

            offset, byte_count = data_chunks[0]
            byte_count -= byte_count % 4
            stream.seek(offset)
            sample_count = finite_count = nonzero_count = nonfinite_count = 0
            square_sum = 0.0
            peak = 0.0
            channel_square = [0.0, 0.0]
            channel_peak = [0.0, 0.0]
            channel_nonzero = [0, 0]
            channel_finite = [0, 0]
            remaining = byte_count
            while remaining >= 4:
                block_bytes = min(1024 * 1024, remaining)
                block_bytes -= block_bytes % 8
                if block_bytes == 0:
                    break
                block = stream.read(block_bytes)
                if not block:
                    break
                block = block[:len(block) - (len(block) % 4)]
                for (value,) in struct.iter_unpack("<f", block):
                    channel = sample_count & 1
                    sample_count += 1
                    if not math.isfinite(value):
                        nonfinite_count += 1
                        continue
                    finite_count += 1
                    channel_finite[channel] += 1
                    absolute = abs(value)
                    square_sum += value * value
                    peak = max(peak, absolute)
                    channel_square[channel] += value * value
                    channel_peak[channel] = max(channel_peak[channel], absolute)
                    if value != 0.0:
                        nonzero_count += 1
                        channel_nonzero[channel] += 1
                remaining -= len(block)

    except (OSError, struct.error, OverflowError) as exc:
        return {"status": "WAV_READ_ERROR", "path": str(path), "error": str(exc)}

    rms = math.sqrt(square_sum / finite_count) if finite_count else 0.0
    channel_rms = [math.sqrt(channel_square[i] / max(1, channel_finite[i])) for i in range(2)]
    if sample_count == 0:
        status = "EMPTY_DUMP"
    elif nonzero_count == 0:
        status = "SILENCE_AT_HOST_MIX"
    else:
        status = "NONZERO_PCM_AT_HOST_MIX"
    return {
        "status": status,
        "path": str(path),
        "file_bytes": file_size,
        "sample_format": "IEEE_FLOAT32",
        "channels": 2,
        "sample_rate_hz": rate,
        "frames": sample_count // 2,
        "samples_analyzed": sample_count,
        "finite_samples": finite_count,
        "nonfinite_samples": nonfinite_count,
        "nonzero_samples": nonzero_count,
        "rms": round(rms, 8),
        "peak": round(peak, 8),
        "channel_rms": [round(value, 8) for value in channel_rms],
        "channel_peak": [round(value, 8) for value in channel_peak],
        "channel_nonzero_samples": channel_nonzero,
        "audible_output": "UNVERIFIED_PHYSICALLY",
        "note": "Nonzero host mix proves PCM reached AudioOut's mixer, not that the speaker played it.",
    }


def parse_audioout_log(text: str) -> dict[str, object]:
    """Extract bounded AudioOut callback and guest-port metrics from captured logs."""
    port_re = re.compile(
        r"\[AudioOut\] port (\d+) type=(-?\d+) fmt=(\d+) ch=(\d+) len=(\d+): "
        r"outputs=(\d+) nonzero_bufs=(\d+) in_rms=([\d.eE+-]+) in_peak=([\d.eE+-]+).*?"
        r"fill=(\d+)/(\d+) underruns=(\d+)"
    )
    host_re = re.compile(
        r"\[AudioOut\] host: running=(\d+) render_calls=(\d+) render_frames=(\d+) "
        r"mix_rms=([\d.eE+-]+) mix_peak=([\d.eE+-]+) dump_frames=(\d+)"
    )
    pacing_re = re.compile(
        r"\[AudioOut\] pacing port (\d+): intervals=(\d+) avg_ms=([\d.eE+-]+) "
        r"max_ms=([\d.eE+-]+) planned_wait_avg_ms=([\d.eE+-]+) queue_min=(\d+) "
        r"adjust_us=([\-\d.eE+]+) resets=(\d+)"
    )
    ports = []
    for match in port_re.finditer(text):
        g = match.groups()
        ports.append({
            "port": int(g[0]), "type": int(g[1]), "format": int(g[2]),
            "channels": int(g[3]), "grain_frames": int(g[4]),
            "outputs": int(g[5]), "nonzero_buffers": int(g[6]),
            "input_rms": float(g[7]), "input_peak": float(g[8]),
            "ring_fill_frames": int(g[9]), "ring_capacity_frames": int(g[10]),
            "underruns": int(g[11]),
        })
    hosts = []
    for match in host_re.finditer(text):
        g = match.groups()
        hosts.append({
            "running": bool(int(g[0])), "render_calls": int(g[1]),
            "render_frames": int(g[2]), "mix_rms": float(g[3]),
            "mix_peak": float(g[4]), "dump_frames": int(g[5]),
        })
    pacing = []
    for match in pacing_re.finditer(text):
        g = match.groups()
        pacing.append({
            "port": int(g[0]), "intervals": int(g[1]), "average_interval_ms": float(g[2]),
            "maximum_interval_ms": float(g[3]), "planned_wait_average_ms": float(g[4]),
            "minimum_queue_frames": int(g[5]), "adjustment_us": float(g[6]),
            "clock_resets": int(g[7]),
        })
    return {
        "port_observation_count": len(ports),
        "latest_port_observations": ports[-25:],
        "host_observation_count": len(hosts),
        "latest_host_observation": hosts[-1] if hosts else None,
        "pacing_observation_count": len(pacing),
        "latest_pacing_observations": pacing[-25:],
        "audioout_open_lines": [line for line in text.splitlines() if "[AudioOut] open:" in line][-25:],
        "host_output_start_seen": "host output started" in text,
        "host_output_suppression_seen": any(
            marker in text for marker in ("PS4_AUDIO_MUTE set", "PS4_AUDIO_NULL set")
        ),
    }


def parse_ajm_decode_log(text: str) -> dict[str, object]:
    """Summarize opt-in decode-stage PCM evidence from the AJM worker."""
    lines = [line for line in text.splitlines() if "[AJM] decode:" in line]
    records = []
    pattern = re.compile(
        r"inst=(0x[0-9a-fA-F]+|\d+) codec=(\d+) fmt=(\d+) ch=(\d+) run=(\d+) "
        r"input_bytes=(\d+) input_used=(\d+) output_bytes=(\d+) frames=(\d+) "
        r"result=(0x[0-9a-fA-F]+) internal=(-?\d+) pcm_nonzero=(\d+) "
        r"pcm_rms=([\d.eE+-]+) pcm_peak=([\d.eE+-]+)"
    )
    for line in lines:
        match = pattern.search(line)
        if not match:
            continue
        g = match.groups()
        result_bits = int(g[9], 16)
        records.append({
            "instance": g[0], "codec": int(g[1]), "format": int(g[2]),
            "channels": int(g[3]), "run_flags": int(g[4]),
            "input_bytes": int(g[5]), "input_used": int(g[6]),
            "output_bytes": int(g[7]), "frames": int(g[8]),
            "result": g[9], "result_bits": result_bits, "internal_result": int(g[10]),
            "pcm_nonzero_samples": int(g[11]), "pcm_rms": float(g[12]),
            "pcm_peak": float(g[13]),
        })
    # Match AJM's decoder_error classification in ps4_ajm.c. In particular,
    # NOT_ENOUGH_ROOM (0x10) is a flow-control result, not a codec failure.
    decoder_error_mask = 0x80000000 | 0x40000000 | 0x00000007
    return {
        "decode_log_lines": len(lines),
        "parsed_decode_records": len(records),
        "latest_decode_record": records[-1] if records else None,
        "decoded_jobs_with_pcm_bytes": sum(record["output_bytes"] > 0 for record in records),
        "decoded_jobs_with_nonzero_pcm": sum(record["pcm_nonzero_samples"] > 0 for record in records),
        "decode_jobs_with_any_nonzero_result": sum(record["result_bits"] != 0 for record in records),
        "decode_jobs_with_error_result": sum(
            bool(record["result_bits"] & decoder_error_mask) for record in records
        ),
        "decode_jobs_with_not_enough_room_result": sum(
            bool(record["result_bits"] & 0x00000010) for record in records
        ),
        "decode_jobs_with_partial_input_result": sum(
            bool(record["result_bits"] & 0x00000008) for record in records
        ),
    }
