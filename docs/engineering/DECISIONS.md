# Engineering decisions

Updated: 2026-10-23.

| Decision | Rationale | Status |
|---|---|---|
| Preserve the existing native ARM64 CPU/runtime. | It reaches both opening cards without requiring Rosetta. | Active |
| Integrate the pinned GoW3-adapted shadPS4 GPU as the target production renderer. | The verified `Sohebdsa/gow3_pc` snapshot carries the GoW3 aliasing, effective depth/stencil, and scaled MIN/MAX fixes. | Approved; isolated native build and Vulkan initialization pass; game dispatch not integrated |
| Keep the custom Metal implementation as a diagnostic fallback. | Current captures show white/half-black output after the opening screens; counters do not establish correctness. | Active |
| Use KosmicKrisp first for candidate Vulkan testing. | It passes the tested source-derived M2 feature/workload checks. | Selected for isolated tests; no game workload yet |
| Keep MoltenVK as a comparison driver only. | It cannot create a strict renderer-compatible device because `robustBufferAccess2` and `nullDescriptor` are missing. | Active |
| Keep one authoritative guest address-space mapping. | Runtime owns `mem_base`; the candidate callback now checks logical allocated extents, returns free ranges as sparse holes, and forwards invalidations. | Contract implemented and synthetic-tested; not installed in game |
| Use a typed per-import bridge for candidate HLE functions. | Direct function pointers from `gow3gpu_resolve` cannot be cast to the runtime's `GuestContext*` shims. | Required before GNM dispatch |
| Keep game assets, generated guest code, and captures local. | These are proprietary or derived game data. | Active; generated local inputs are ignored/untracked |

## Target ownership

| Subsystem | Owner after integration |
|---|---|
| Recompiled CPU, guest VA allocation, nongraphics HLE | Existing native ARM64 runtime |
| GNM import handling, PM4, shaders, texture/image cache, render state | Pinned GoW3/shadPS4 GPU |
| Guest memory map and CPU/GPU page events | Runtime map plus explicit versioned callbacks |
| GPU EOP, event queue and guest wakeup | One tested completion bridge |
| VideoOut and presentation | One integrated presenter |
| Existing Metal path | Differential diagnostics only |
