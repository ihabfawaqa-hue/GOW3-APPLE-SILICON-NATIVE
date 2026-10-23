# Test matrix

Updated: 2026-10-23. Status values are evidence-based; **NOT TESTED** is not a pass.

| Gate | Test | Status | Evidence / remaining work |
|---|---|---|---|
| M0 | Existing executable architecture | **PASS** | `file` and `lipo -info ps4_app` report Mach-O arm64. |
| M0 | Source-driven Ninja graph regeneration | **PASS** | Two consecutive generations produced SHA-256 `7a844db1e8d091d830eba76a6bcf61fb083a8300d8a4430cb5049e80a816066b`; build-graph tests verify unique outputs, language rules, and Ninja parsing. |
| M0 | ARM64 link after regeneration | **PASS** | `ninja -j4 ps4_app` completed as a link-only step; `file ps4_app` and `lipo -info ps4_app` both confirm arm64. |
| M0 | Incremental build after a source edit | **NOT TESTED** | Must edit/restore one source in an isolated worktree and confirm only its object and final link rebuild. |
| M0 | Isolated clean build | **NOT TESTED** | Requires a local worktree plus links to the machine-local guest code inputs. |
| M1 | Sony and Santa Monica splash rendering | **PARTIAL** | User-reported baseline; internal reference captures are not yet verified. |
| M1 | Bounded smoke run and internal capture | **PARTIAL** | `python3 scripts/lab/iterate.py --mode smoke --runtime-seconds 45 --no-progress-seconds 25 --max-log-mb 32 --max-capture-mb 512` completed at deadline; 42 frames and a partial draw snapshot were saved under Application Support. Latest frame analysis: all black; both reported splash screens remain unverified by this run. |
| M2 | GoW3 image-alias regression | **PASS** | Upstream `tests/test_aliasing.cpp`, compiled with `aliasing.cpp` and `host_compatibility.cpp` using native `clang++ -std=c++23`; output `aliasing-test: OK`; binary is Mach-O arm64. |
| M2 | GoW3 scaled MIN/MAX blend regression | **PASS** | Upstream `tests/test_blend_rewrite.cpp`, native arm64 C++23; output `blend-rewrite-test: OK`. |
| M2 | GoW3 effective depth/stencil regression | **PASS** | Upstream `tests/test_depth_stencil_state.cpp`, native arm64 C++23; output `depth-stencil-state-test: OK`. |
| M2 | Full GoW3/shadPS4 video core on ARM64 | **BLOCKED** | Exact source configure stops at missing `Vulkan_LIBRARY`; no GPU runtime has been linked or initialized. |
| M2 | C bridge initialization and teardown | **FAIL** | Existing bridge is a forwarding placeholder; no standalone GPU lifecycle. |
| M3 | KosmicKrisp Vulkan feature/workload probe | **BLOCKED** | No SDK loader or driver is installed; upstream CMake cannot find `Vulkan_LIBRARY`. |
| M3 | MoltenVK comparison probe | **BLOCKED** | No SDK loader or driver is installed. |
| M4 | Guest GNM submission reaches shadPS4 GPU | **FAIL** | Source call graph reaches custom Metal directly. |
| M5 | Guest memory, page faults, EOP and equeues | **NOT TESTED** | No integrated GPU memory/completion contract. |
| M6 | Shader/texture correctness against captured guest resources | **NOT TESTED** | Existing texture and shader probes are incomplete and not full render tests. |
| M7 | Deterministic GPU replay | **NOT TESTED** | Current utility decodes synthetic PM4 packet structure only. |
| M8–M9 | Correct geometry, textures, multipass composition | **NOT TESTED** | No validated guest GPU replay or trusted image comparison. |
| M10 | Recognizable post-splash image | **FAIL** | Latest bounded run capture is an all-black frame; no recognizable image is verified. |
| M11–M14 | Audio/video, input, gameplay, performance | **NOT TESTED** | Must follow graphics correctness. |

Available local fixture tests are diagnostic tools; a synthetic descriptor, packet, or shader fixture does not prove game rendering.
