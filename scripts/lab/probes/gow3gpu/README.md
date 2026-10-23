# Standalone GoW3 GPU C ABI smoke

This smoke dynamically loads the native `gow3gpu` shared library, calls `gow3gpu_register_kernel()`, verifies a known imported NID resolves, and checks an unknown NID returns zero.

```sh
cmake -S scripts/lab/probes/gow3gpu -B /tmp/gow3_gpu_abi_smoke_build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gow3_gpu_abi_smoke_build -j4
/tmp/gow3_gpu_abi_smoke_build/gow3gpu-abi-smoke /path/to/libgow3gpu.dylib
```

`runtime_contract_stubs.cpp` exists only to satisfy `dlopen` for this isolated registration test. Its functions do not model guest memory, page faults, timing, or restart behavior and must never be linked into `ps4_app`.

## Renderer initialization smoke

On macOS, this separate executable installs a synthetic empty guest-memory API, calls `gow3gpu_init`, constructs the pinned Vulkan presenter, registers the kernel service, then checks the GNM symbol table. It submits no guest work and does not validate renderer teardown.

```sh
cmake --build /tmp/gow3gpu-arm64-build --parallel 2
cmake --build /tmp/gow3_gpu_abi_smoke_build --parallel 2 --target gow3gpu-renderer-init-smoke
VK_DRIVER_FILES=/path/to/kosmickrisp_mesa_icd.aarch64.json \
  /tmp/gow3_gpu_abi_smoke_build/gow3gpu-renderer-init-smoke \
  /tmp/gow3gpu-arm64-build/libgow3gpu.dylib
```

The current M2 run passes with KosmicKrisp and selects the Apple M2 device. Its logs and result JSON are in `~/Library/Application Support/GOW3Lab/runs/gow3gpu_init_20261023_m2_kosmickrisp/`. The native renderer needs an SDL Cocoa window and `CAMetalLayer`; its macOS port creates those on the process main thread and pumps SDL events on the main dispatch queue. SDL's own Vulkan window path is not used; the pinned renderer creates `VK_EXT_metal_surface` from the SDL Metal layer.

The candidate C ABI still exposes no shutdown function. The smoke process exits immediately after confirming initialization because the window timer and kernel service have no coordinated teardown yet. The first failed attempts and their logs are retained beside the passing run for diagnosis.
