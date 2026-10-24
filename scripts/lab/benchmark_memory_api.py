#!/usr/bin/env python3
"""Bounded, frame-clock-input A/B harness for immutable guest-memory API reads.

Artifacts remain outside Git. Each mode uses the same candidate binary and caches.
The 500-flip measurement segment begins after 20 consecutive frames above 500 draws.
Compare frame IDs, draw counts and screenshots before treating timing as comparable.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["locked", "published"], required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--library", type=Path, default=Path('/tmp/gow3gpu-arm64-dense-range-build/libgow3gpu.dylib'))
    parser.add_argument("--runtime-seconds", type=int, default=300)
    args = parser.parse_args()
    if Path(args.name).name != args.name:
        parser.error("name must be a single path component")
    repo = Path(__file__).resolve().parents[2]
    lab = Path.home()/'Library/Application Support/GOW3Lab'
    runs = lab/'runs'
    out = lab/'spartan-optimization'/args.name
    out.mkdir(parents=True, exist_ok=False)
    csv_path = out/'flips.csv'
    before = set(runs.iterdir())
    env = os.environ.copy()
    env.update({
        'PS4_GOW3GPU_BACKEND': 'gow3gpu-vulkan',
        'PS4_GOW3GPU_LIBRARY': str(args.library.resolve()),
        'VK_DRIVER_FILES': env.get('VK_DRIVER_FILES', str(lab/'runtime/current/kosmickrisp_icd.json')),
        'PS4_PAD_AUTOPRESS': '4000', 'PS4_PAD_CLOCK': 'frames',
        'GOW3_LOCK_GUEST_MEMORY_API': '1' if args.mode == 'locked' else '0',
        'GOW3_BENCHMARK_CSV': str(csv_path), 'GOW3_FRAME_STATS': '0',
    })
    command = [sys.executable, str(repo/'scripts/lab/iterate.py'), '--skip-build',
               '--runtime-seconds', str(args.runtime_seconds), '--no-progress-seconds', '90',
               '--max-log-mb', '48', '--max-capture-mb', '384',
               '--frame-dump-every', '1000000', '--frame-dump-max', '3',
               '--pass-capture-every', '1000000', '--pass-capture-max', '3']
    run = None
    anchor = None
    consecutive = 0
    processed = 0
    prev = None
    captured = set()
    start = time.monotonic()
    stopped = False
    trace_process = None
    trace_log = None
    with (out/'harness.log').open('w') as log:
        process = subprocess.Popen(command, cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                if run is None:
                    new = set(runs.iterdir()) - before
                    if new:
                        run = max(new, key=lambda p: p.stat().st_mtime)
                if csv_path.exists():
                    with csv_path.open() as stream:
                        rows = list(csv.DictReader(stream))
                    for row in rows[processed:]:
                        if not row.get('compiles'):
                            break  # a partial final write will be consumed on the next poll
                        flip = int(row['flip'])
                        draws = int(row['draws'])
                        if prev is not None:
                            consecutive = consecutive + 1 if draws - prev > 500 else 0
                        prev = draws
                        processed += 1
                        if anchor is None and consecutive >= 20:
                            anchor = flip
                            print(f'{args.name}: heavy-scene anchor flip {anchor}', flush=True)
                            if env.get('GOW3_BENCH_TRACE') == '1':
                                target = subprocess.run(['pgrep', '-P', str(process.pid), '-x', 'ps4_app'],
                                                        text=True, capture_output=True).stdout.strip()
                                if target and target.isdigit():
                                    trace_log = (out/'trace-runner.log').open('w')
                                    trace_process = subprocess.Popen([
                                        sys.executable, str(repo/'scripts/lab/record_metal.py'),
                                        '--pid', target, '--duration-seconds', '8',
                                        '--output', str(out/'metal.trace')], stdout=trace_log,
                                        stderr=subprocess.STDOUT)
                        if anchor is not None and run is not None:
                            for offset in (150, 350):
                                if flip >= anchor + offset and offset not in captured:
                                    (run/'candidate-frame-dump.trigger').touch()
                                    (run/'candidate-pass-trace.trigger').touch()
                                    if env.get('GOW3_SCENE_DEBUG'):
                                        Path(env['GOW3_SCENE_DEBUG']).touch()
                                    if env.get('MESA_KK_CAPTURE_TRIGGER'):
                                        Path(env['MESA_KK_CAPTURE_TRIGGER']).touch()
                                    captured.add(offset)
                            if flip >= anchor + 500 and not stopped:
                                process.send_signal(signal.SIGINT)
                                stopped = True
                if time.monotonic() - start > args.runtime_seconds + 60:
                    raise TimeoutError('Harness failed to finish within its watchdog')
                if shutil.disk_usage(lab).free < 6 * 1024**3:
                    raise RuntimeError('Stopping benchmark to preserve 6 GiB of free disk')
                time.sleep(0.5)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=10)
    report = {'mode': args.mode, 'run': str(run) if run else None, 'anchor_flip': anchor,
              'last_flip': processed, 'elapsed_seconds': time.monotonic()-start,
              'harness_exit': process.returncode, 'library': str(args.library.resolve()),
              'captures_after_anchor': sorted(captured), 'stopped_after_500_flips': stopped}
    if trace_process:
        try:
            trace_process.wait(timeout=45)
        except subprocess.TimeoutExpired:
            trace_process.send_signal(signal.SIGINT)
        if trace_log: trace_log.close()
    (out/'benchmark.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2), flush=True)
    return 0 if anchor is not None and processed >= anchor + 500 else 1


if __name__ == '__main__':
    raise SystemExit(main())
