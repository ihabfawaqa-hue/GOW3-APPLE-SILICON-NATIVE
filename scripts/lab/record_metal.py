#!/usr/bin/env python3
"""Short attach-only Metal trace with guards on free disk and Instruments' hidden spool.

Never terminates the target app. A final .trace size limit alone is insufficient:
Instruments accumulates instruments*.ktrace in the per-user temporary directory.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time


def spool_sizes():
    result = {}
    for path in Path(tempfile.gettempdir()).glob("instruments*.ktrace"):
        try:
            result[str(path)] = path.stat().st_size
        except FileNotFoundError:
            pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=float, default=8)
    parser.add_argument("--min-free-gb", type=float, default=6)
    parser.add_argument("--max-spool-mb", type=float, default=1024)
    args = parser.parse_args()
    if not 1 <= args.duration_seconds <= 30 or args.min_free_gb < 2 or args.max_spool_mb < 64:
        parser.error("Duration must be 1–30 s, reserve at least 2 GiB, spool budget at least 64 MiB")
    output = args.output.expanduser().resolve()
    if output.suffix != ".trace" or output.exists():
        parser.error("Output must be a new .trace path")
    os.kill(args.pid, 0)
    output.parent.mkdir(parents=True, exist_ok=True)
    temp_path = Path(tempfile.gettempdir())
    reserve = args.min_free_gb * 1024**3
    if min(shutil.disk_usage(output.parent).free, shutil.disk_usage(temp_path).free) < reserve:
        parser.error("Insufficient free disk space for the requested reserve")
    before = spool_sizes()
    started = time.monotonic()
    reason = "time_limit"
    stopped_at = None
    peak_growth = 0
    command = ["xcrun", "xctrace", "record", "--template", "Metal System Trace",
               "--attach", str(args.pid), "--time-limit", f"{round(args.duration_seconds * 1000)}ms",
               "--output", str(output), "--no-prompt"]
    with output.with_suffix(".recording.log").open("w") as log:
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            while proc.poll() is None:
                now = time.monotonic()
                growth = sum(max(0, size - before.get(path, 0)) for path, size in spool_sizes().items())
                peak_growth = max(peak_growth, growth)
                free = min(shutil.disk_usage(output.parent).free, shutil.disk_usage(temp_path).free)
                if stopped_at is None:
                    if free < reserve:
                        reason = "disk_reserve"
                    elif growth > args.max_spool_mb * 1024**2:
                        reason = "spool_budget"
                    elif now - started > args.duration_seconds + 60:
                        reason = "watchdog"
                    if reason != "time_limit":
                        proc.send_signal(signal.SIGINT)
                        stopped_at = now
                elif now - stopped_at > 45:
                    proc.terminate()
                    proc.wait(timeout=10)
                    break
                time.sleep(0.25)
        except BaseException:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=45)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    proc.wait(timeout=10)
            raise
    report = {"target_pid": args.pid, "returncode": proc.returncode, "stop_reason": reason,
              "elapsed_seconds": time.monotonic() - started, "peak_spool_growth_bytes": peak_growth,
              "output": str(output), "spool_directory": str(temp_path),
              "note": "The target app was left running. Verify the saved trace with xctrace export --toc."}
    output.with_suffix(".recording.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if proc.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
