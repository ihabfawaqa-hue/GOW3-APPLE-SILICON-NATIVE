# Engineering decisions

Updated: 2026-10-23.

| Decision | Rationale | Status |
|---|---|---|
| Preserve the existing native ARM64 CPU/runtime. | The current binary runs natively on Apple Silicon and reaches the opening sequence. | Active |
| Make the GoW3-adapted shadPS4 video core the target production GPU owner. | The exact candidate is `Sohebdsa/gow3_pc` commit `b907d8a9f2324ba5f514fbfe7de803e027c6ca26`, with shadPS4 base `b4e7ae776071ceb1dd35004577a8719481e1d258`; it contains the GoW3 aliasing, effective depth/stencil, and scaled MIN/MAX fixes. | Approved architecture; source verified; import/build pending |
| Keep custom Metal as a diagnostic comparison backend. | Its current PM4 and texture path produces corrupted geometry and textures; it is not a correctness oracle. | Active |
| Do not route through the existing `gow3gpu_bridge.cpp` as-is. | It is a placeholder with fabricated NIDs, no runtime callers, an in-memory map that does not own guest memory, and forwarding back to Metal. | Active |
| Keep the vendored shadPS4 shader recompiler separate from the full GPU backend. | Its GCN-to-SPIR-V frontend can be useful, but its current MSL integration cannot replace the Vulkan GPU resource and synchronization layers. | Active |
| Test KosmicKrisp first and MoltenVK independently. | Driver selection must be based on required semantics and representative workload tests, not advertised extension names. The M2 host runs macOS 27.0, but has no Vulkan SDK/loader installed. | Pending isolated SDK/runtime and source-derived probe |
| Keep local game inputs and captures out of version control. | The user supplied paths contain proprietary assets and derived guest code. | Active; enforced by `.gitignore` |

## Ownership target

| Subsystem | Owner after integration |
|---|---|
| Recompiled CPU, HLE, guest address-space mapping | Existing ARM64 runtime |
| GNM imports, PM4, shader compilation, texture/image cache, render state | Imported GoW3/shadPS4 GPU |
| GPU completion and interrupts | One explicit bridge contract with the existing equeue/thread runtime |
| Page tracking | Shared guest-memory API; runtime owns mappings, GPU cache subscribes to invalidations |
| Presentation | Integrated VideoOut path with one renderer owning each presented frame |
| Custom Metal renderer | Diagnostic/differential backend only |

Each shared contract still needs concrete API and synchronization tests before integration.
