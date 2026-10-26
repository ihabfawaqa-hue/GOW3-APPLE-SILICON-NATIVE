# Documentation index

This project was developed in short, measured iterations, and the notes here are the working record.
They are dated snapshots: later commits may have changed things, so check the history before relying on
an older statement.

## Start here

| Document | What it tells you |
|---|---|
| [`engineering/STATUS.md`](engineering/STATUS.md) | Where the project stands |
| [`engineering/DECISIONS.md`](engineering/DECISIONS.md) | What was tried, kept and rejected, and why |
| [`engineering/BLOCKERS.md`](engineering/BLOCKERS.md) | Verified blockers at the time of writing |
| [`engineering/TEST_MATRIX.md`](engineering/TEST_MATRIX.md) | What is tested and how |

## Profiling and performance

| Document | Subject |
|---|---|
| [`engineering/SPARTAN_PROFILE_20261024.md`](engineering/SPARTAN_PROFILE_20261024.md) | First measured profile of the frame |
| [`engineering/STRUCTURAL_BOTTLENECK_20261024.md`](engineering/STRUCTURAL_BOTTLENECK_20261024.md) | Lock contention and GPU-thread waits |
| [`engineering/SUBMIT_COALESCING_PROPOSAL_20261026.md`](engineering/SUBMIT_COALESCING_PROPOSAL_20261026.md) | Submit counts and why the first proposal was withdrawn |
| [`engineering/METAL_ENCODERS_20261026.md`](engineering/METAL_ENCODERS_20261026.md) | Metal encoders per frame, tessellation, specialization cost |
| [`engineering/AB_ARGON_20261026.md`](engineering/AB_ARGON_20261026.md) | Alternating A/B of the 2026-10-26 changes |
| [`engineering/PERF_RESOLUTION_MATRIX.md`](engineering/PERF_RESOLUTION_MATRIX.md) | Resolution and performance matrix |
| [`engineering/OPERATION_SPARTAN_FORENSIC_REPORT_20261026.pdf`](engineering/OPERATION_SPARTAN_FORENSIC_REPORT_20261026.pdf) | An external, AI-generated performance analysis. Many of its claims did not hold for the code; the documents above record which ones. |

## Upstream and provenance

[`engineering/GPU_UPSTREAM_DELTA.md`](engineering/GPU_UPSTREAM_DELTA.md) and
[`engineering/UPSTREAM_GPU_MANIFEST.md`](engineering/UPSTREAM_GPU_MANIFEST.md) record where the GPU code
comes from and how it differs. [`gow3/ROADMAP.md`](gow3/ROADMAP.md) is the roadmap inherited from the
Windows port.

## Handoff notes

[`handoff/`](handoff/) and [`engineering/HANDOFF_CLAUDE.md`](engineering/HANDOFF_CLAUDE.md) are notes
written to pass work between sessions, several of them with AI coding assistants. They use work-stream
labels: **Operation Spartan** is the performance investigation, and **Luna** and **Codex** name the
runtime/CPU/audio and GPU/Vulkan work streams. They are kept for the history; the files above are the
cleaner summary. `handoff/bench/` holds the benchmark scripts (see the README's *Diagnostics* section).

## Local paths

Paths in these notes were generalized: `<repo>` is your checkout, `$GOW3_DIR` is your game dump
directory, and `~/Library/Application Support/GOW3Lab/` holds runs and installed runtimes.
