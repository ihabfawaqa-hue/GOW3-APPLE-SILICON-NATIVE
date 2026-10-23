# Isolated GoW3 GPU source import

This directory is an exact, unmodified source snapshot used for the macOS integration experiment. It is not linked into `ps4_app` yet.

| Item | Source |
|---|---|
| Upstream | `https://github.com/Sohebdsa/gow3_pc`, branch `gow3` |
| Repository commit | `b907d8a9f2324ba5f514fbfe7de803e027c6ca26` |
| shadPS4 base | `b4e7ae776071ceb1dd35004577a8719481e1d258`, as recorded in `gpu/VENDOR.txt` |
| Copied paths | `gpu/`, `src/`, `tests/`, root `LICENSE`, root `README.md` |
| Local patches | None at import time |
| License | GPU project source is GPL-2.0-or-later; retain upstream file headers and all bundled third-party license notices |
| Upstream build | CMake, C++23, shared `gow3gpu`; Vulkan required, plus fmt, Boost, tsl-robin-map, VMA, xxhash, SDL3, Zydis, glslang, and platform window dependencies |
| Target under evaluation | macOS 27.0, Apple arm64/M2 |
| Build status | Full target not built: first configure stops at missing Vulkan loader library; upstream CMake also currently treats Apple as non-Windows and requests X11 |

The three requested GoW3 regression tests are present in `tests/` and passed using the native Apple C++23 compiler before and after the source snapshot was copied. They are CPU-side state tests; passing them is not evidence of a working Vulkan device or a rendered GOW3 frame.

The existing application uses a different placeholder at `gpu/gow3gpu_bridge.cpp`. No symbol or lifecycle from that placeholder is reused here. Integration must adapt the exact exported C ABI from `gpu/gow3gpu.h`, preserve one guest-memory owner, and route actual guest GNM submissions through this implementation.

The repository-level `docs/engineering/UPSTREAM_GPU_MANIFEST.md` records the candidate commit, the three required GoW3 fix commits, license provenance, and the separate local shader-recompiler vendor snapshot.
