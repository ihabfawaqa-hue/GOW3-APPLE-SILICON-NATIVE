#!/usr/bin/env python3
"""Analyze exported xctrace tables without double-counting overlapping GPU stages.

Exports stay local. Join encoder metadata by (process, command buffer, encoder),
never by nearest timestamp or by the presentation frame guessed by Instruments.
"""
import argparse
import bisect
import collections
import json
import re
import xml.etree.ElementTree as ET


def rows(path):
    refs = {}
    columns = []
    for _, node in ET.iterparse(path, events=("end",)):
        if node.tag == "mnemonic":
            columns.append(node.text)
        if node.tag != "row":
            continue
        values = []
        for child in node:
            for item in child.iter():
                if "id" in item.attrib:
                    refs[item.attrib["id"]] = item
            values.append(refs.get(child.get("ref"), child))
        yield dict(zip(columns, values))
        node.clear()


def value(row, key):
    node = row.get(key)
    return (node.text or node.get("fmt", "")) if node is not None else ""


def union(intervals):
    merged = []
    for start, end in sorted(intervals):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def length(intervals):
    return sum(end - start for start, end in union(intervals))


def intersection(a, b):
    a, b = union(a), union(b)
    i = j = total = 0
    while i < len(a) and j < len(b):
        total += max(0, min(a[i][1], b[j][1]) - max(a[i][0], b[j][0]))
        if a[i][1] <= b[j][1]:
            i += 1
        else:
            j += 1
    return total


def identity(row):
    return tuple(value(row, key) for key in ("process", "cmdbuffer-id", "encoder-id"))


def analyze(gpu_path, encoder_path=None, process="ps4_app", start_ns=None, end_ns=None,
            object_labels_path=None):
    labels = {}
    object_labels = collections.defaultdict(list)
    if object_labels_path:
        for row in rows(object_labels_path):
            key = (value(row, "pid"), value(row, "object-id"))
            object_labels[key].append((int(value(row, "timestamp")), value(row, "label")))
        for history in object_labels.values():
            history.sort()
    if encoder_path:
        for row in rows(encoder_path):
            labels[identity(row)] = value(row, "encoder-label")
    game, other = [], []
    stages = collections.defaultdict(list)
    families = collections.defaultdict(list)
    encoder_intervals = collections.defaultdict(list)
    stage_encoders = collections.defaultdict(set)
    matched = []
    matched_encoders = set()
    for row in rows(gpu_path):
        if value(row, "state") != "Active":
            continue
        start = int(value(row, "start"))
        end = start + int(value(row, "duration"))
        if start_ns is not None:
            start = max(start, start_ns)
        if end_ns is not None:
            end = min(end, end_ns)
        if end <= start:
            continue
        interval = (start, end)
        if process not in value(row, "process"):
            other.append(interval)
            continue
        game.append(interval)
        stages[value(row, "channel-name")].append(interval)
        key = identity(row)
        encoder_intervals[key].append(interval)
        stage_encoders[value(row, "channel-name")].add(key)
        label = labels.get(key)
        if not label:
            pid = re.search(r"\((\d+)\)", key[0])
            history = object_labels.get((pid.group(1), key[2]), []) if pid else []
            index = bisect.bisect_right(history, (start, chr(0x10ffff))) - 1
            if index >= 0:
                label = history[index][1]
        if label and re.search(r"\d+x\d+s\d+:", label):
            # Addresses identify backing allocations, not stable pass families.
            family = re.sub(r"\b[0-9a-f]+:(?=\d+x\d+s)", "", label)
            families[family].append(interval)
            matched.append(interval)
            matched_encoders.add(key)
    if not game:
        raise ValueError("No attributed active GPU intervals in the selected window")
    game = union(game)
    lo = start_ns if start_ns is not None else game[0][0]
    hi = end_ns if end_ns is not None else game[-1][1]
    gaps = [(a[1], b[0]) for a, b in zip(game, game[1:]) if b[0] > a[1]]
    # Include explicit window edges when provided.
    if game[0][0] > lo:
        gaps.insert(0, (lo, game[0][0]))
    if game[-1][1] < hi:
        gaps.append((game[-1][1], hi))
    busy = length(game)
    ranked = sorted(families, key=lambda key: length(families[key]), reverse=True)
    return {
        "window_ns": [lo, hi], "window_ms": (hi-lo)/1e6,
        "game_busy_ms": busy/1e6, "game_inactive_ms": length(gaps)/1e6,
        "other_gpu_work_during_game_gaps_ms": intersection(gaps, other)/1e6,
        "no_attributed_gpu_work_during_game_gaps_ms": (length(gaps)-intersection(gaps, other))/1e6,
        "game_busy_percent": 100*busy/(hi-lo),
        "stage_union_ms_nonadditive": {key: length(val)/1e6 for key, val in stages.items()},
        "unique_metal_encoders_all_types": len(encoder_intervals),
        "unique_render_encoders": len(stage_encoders["Vertex"] | stage_encoders["Fragment"]),
        "unique_compute_blit_encoders": len(stage_encoders["Compute"]),
        "label_coverage_percent_of_game_busy": 100*length(matched)/busy,
        "matched_encoders": len(matched_encoders),
        "top_pass_families": [{"family": key, "busy_union_ms_nonadditive": length(families[key])/1e6} for key in ranked[:10]],
        "longest_game_gaps": [{"start_ns": a, "duration_ms": (b-a)/1e6,
                               "other_gpu_ms": intersection([(a,b)], other)/1e6}
                              for a,b in sorted(gaps, key=lambda gap: gap[1]-gap[0], reverse=True)[:20]],
        "limits": ["Game inactivity is not necessarily device idleness or a CPU stall.",
                   "Stage and family unions overlap and must not be added.",
                   "Encoder counts include compute/blit and render encoders.",
                   "No frame-time percentiles without independently verified presentation boundaries.",
                   "Unmatched labels are unclassified; partial coverage cannot establish a top-ten ranking."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gpu_xml")
    parser.add_argument("--encoders")
    parser.add_argument("--object-labels", help="metal-object-label export; timestamped encoder label fallback")
    parser.add_argument("--process", default="ps4_app")
    parser.add_argument("--start-ns", type=int)
    parser.add_argument("--end-ns", type=int)
    args = parser.parse_args()
    print(json.dumps(analyze(args.gpu_xml, args.encoders, args.process, args.start_ns, args.end_ns,
                             args.object_labels), indent=2))


if __name__ == "__main__":
    main()
