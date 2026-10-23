# God of War III Remastered (CUSA01623) Native ARM64 Port Roadmap

## Executive Overview

Project Spartan executes God of War III Remastered natively on Apple Silicon ARM64 by pairing:
1. **CPU Subsystem**: 100% Ahead-Of-Time (AOT) recompiled native Mach-O ARM64 instructions (PS4Recomp), with all JIT emulation (FEXCore, Rosetta 2) strictly eliminated.
2. **GPU Subsystem**: Dual-backend graphics architecture:
   - **Primary Backend (Metal)**: Direct PM4-to-Metal translation via `runtime/modules/libSceGnmDriver/ps4_gnm_metal.mm` with real GCN shader recompilation and texture descriptor decoding.
   - **Secondary / Migration Backend (shadPS4 Vulkan / MoltenVK)**: God of War III–adapted video core via the C interface bridge `gpu/gow3gpu.h` (referenced from `Sohebdsa/gow3_pc` / `SpartanPort`).

---

## Migration Milestones & Timeline

### Milestone 1: Dual-Backend Architecture & Bridge Integration (Completed)
- [x] Standard C bridge interface `gpu/gow3gpu.h` defined with configuration, submission, presentation, and telemetry APIs.
- [x] Provenance and vendor tracking established in `gpu/VENDOR.txt`.
- [x] Bridge implementation `gpu/gow3gpu_bridge.cpp` connecting runtime dispatching with clean shutdown and idle synchronization.
- [x] Fix undefined symbol `_ps4_gnm_metal_destroy` in `ps4_gnm_metal.mm` with full GPU idle wait and cache clearance.
- [x] Correct format parameter order (`unique_bins` preceding `first_pixel`) in `ps4_metal_screen.m`.
- [x] Automated test suites passing: `tests/test_texture_descriptor_decoding.py` (5/5) and `tests/test_driver_linkage.py` (4/4).

### Milestone 2: Memory Hierarchy & Page Fault Optimization (Target: Round 9)
- [ ] Implement the aligned 256 KB fault-window unprotect (`GOW3_FAULT_WINDOW=256`) in the native signal handler.
- [ ] Configure `GOW3_DMEM_MB` to allocate 5056 MiB direct memory arena matching retail PS4 Liverpool address space.
- [ ] Pre-populate the 512 MiB persistent staging buffer pool (`GOW3_STAGING_KEEP_MB=512`) in Apple Silicon unified memory (UMA).
- [ ] Pre-touch DMA buffers for asset streaming reads to eliminate Unix `EFAULT` kernel errors.

### Milestone 3: GCN Sea Islands Shader Translation & Pipeline Binding (Target: Round 10)
- [ ] Complete SPIR-V generation for all Liverpool vertex, pixel, and compute shaders via shadPS4 shader recompiler.
- [ ] Feed SPIR-V into SPIRV-Cross MSL backend for Metal pipeline caching.
- [ ] Validate vertex attribute layouts against authentic Liverpool registers (`SPI_VS_OUT_CONFIG`, `VGT_NUM_INDICES`).
- [ ] Ensure non-draining `IT_SET_PREDICATION` processing across all submissions.

### Milestone 4: Authentic In-Engine Rendering Verification (Target: Round 11)
- [ ] Verify authentic in-engine frame capture with the statistical image classifier in `ps4_metal_screen.m` (rejecting flat/grayscale polygons).
- [ ] Confirm proper texture mapping across character models (Kratos, Poseidon), environment geometry, and UI widgets.
- [ ] Validate camera MVP matrices derived from constant buffers without arbitrary scaling.

### Milestone 5: Audio & Video Media Subsystems (Target: Round 12)
- [ ] Implement `sceVideodecQueryResourceInfo` and hardware accelerated H.264/AVC decoding via VideoToolbox for opening cinematics.
- [ ] Complete `libSceAjm` (Audio Joint Module) AT9 / PCM audio output routing to CoreAudio.
- [ ] Verify 60 FPS gameplay stability on Apple Silicon M2.
