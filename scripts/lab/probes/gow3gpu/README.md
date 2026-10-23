# Standalone GoW3 GPU C ABI smoke

These smokes dynamically load the native `gow3gpu` shared library. The ABI smoke installs synthetic host-runtime callbacks before starting the kernel service, verifies a known imported NID resolves, and checks an unknown NID returns zero. The host-runtime smoke tests ABI validation and forwarding for errno, clocks, VFS paths, host-thread attachment, restart requests, optimization flags, and recoverable GPU faults.

```sh
cmake -S scripts/lab/probes/gow3gpu -B /tmp/gow3_gpu_abi_smoke_build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gow3_gpu_abi_smoke_build -j4
/tmp/gow3_gpu_abi_smoke_build/gow3gpu-abi-smoke /path/to/libgow3gpu.dylib
/tmp/gow3_gpu_abi_smoke_build/gow3gpu-host-runtime-smoke /path/to/libgow3gpu.dylib
/tmp/gow3_gpu_abi_smoke_build/gow3gpu-runtime-adapter-smoke
/tmp/gow3_gpu_abi_smoke_build/gow3gpu-host-page-smoke
```

`gow3gpu-runtime-adapter-smoke` compiles the runtime adapter against a synthetic `GuestContext` and exercises checked guest reads/writes, atomic synchronization, host callbacks, map/unmap/invalidate notifications, and host page protection. The image/argument prefix and non-free VM extents are accepted; free extents and ranges crossing into holes are rejected, while the sparse-read API reports holes for zero-fill. `gow3gpu-host-page-smoke` compares the vendored tracker page size with the running macOS page size; on the M2 both report 16 KiB. These are contract tests and do not submit real game commands.

The candidate shared library owns its internal `runtime_*` entry points and routes host-owned services through the versioned `Gow3GpuHostRuntimeApi`. The `errno_address` callback supplies the host C-library thread's errno slot, not PS4 guest TLS. No executable-level `runtime_*` stubs are used. The smoke callbacks are test-only and do not model the primary guest-memory runtime. Configure the candidate GPU with `-DGOW3GPU_TEST_HOOKS=ON` to include the fault-recovery self-test hook; it is disabled in production builds.

## Renderer initialization smoke

On macOS, this separate executable installs a synthetic empty guest-memory API and synthetic host-runtime API, calls `gow3gpu_init`, constructs the pinned Vulkan presenter, registers the kernel service, then checks the GNM symbol table. It submits no guest work and does not validate renderer teardown.

```sh
cmake --build /tmp/gow3gpu-arm64-build --parallel 2
cmake --build /tmp/gow3_gpu_abi_smoke_build --parallel 2 --target gow3gpu-renderer-init-smoke
VK_DRIVER_FILES=/path/to/kosmickrisp_mesa_icd.aarch64.json \
  /tmp/gow3_gpu_abi_smoke_build/gow3gpu-renderer-init-smoke \
  /tmp/gow3gpu-arm64-build/libgow3gpu.dylib
```

The current M2 run passes with KosmicKrisp and selects the Apple M2 device. Its logs and result JSON are in `~/Library/Application Support/GOW3Lab/runs/gow3gpu_init_20261023_m2_kosmickrisp/`. The native renderer needs an SDL Cocoa window and `CAMetalLayer`; its macOS port creates those on the process main thread and pumps SDL events on the main dispatch queue. SDL's own Vulkan window path is not used; the pinned renderer creates `VK_EXT_metal_surface` from the SDL Metal layer.

The candidate C ABI still exposes no shutdown function. The smoke process exits immediately after confirming initialization because the window timer and kernel service have no coordinated teardown yet. The first failed attempts and their logs are retained beside the passing run for diagnosis.
