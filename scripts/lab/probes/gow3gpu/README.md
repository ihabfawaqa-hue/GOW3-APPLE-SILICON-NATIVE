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

## Renderer initialization and GNM submit smoke

On macOS, this separate executable installs synthetic empty guest-memory and host-runtime APIs, calls `gow3gpu_init`, constructs the pinned Vulkan presenter, registers the kernel service, and checks the GNM symbol table. It then resolves the candidate `sceGnmSubmitCommandBuffers` and `sceGnmSubmitDone` NIDs, submits a valid two-DWORD PM4 NOP buffer, calls submit-done, and uses the local diagnostic `gow3gpu_wait_idle` export to confirm that Liverpool retires the queued command.

This verifies the candidate's raw HLE function ABI and queue path. It does not invoke the primary `GuestContext*` import shim, exercise real game memory, validate EOP/equeue delivery, or represent a game workload. It does not validate renderer teardown.

```sh
cmake --build /tmp/gow3gpu-runtime-int-build --parallel 2
cmake --build /tmp/gow3_gpu_runtime_smoke_build --parallel 2 --target gow3gpu-renderer-init-smoke
VK_DRIVER_FILES=/tmp/mesa-kosmickrisp-install312/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
  /tmp/gow3_gpu_runtime_smoke_build/gow3gpu-renderer-init-smoke \
  /tmp/gow3gpu-runtime-int-build/libgow3gpu.dylib
```

The bounded M2/KosmicKrisp result, stdout/stderr, build logs, and arm64 check are in `/Users/ihabfawaqa/Library/Application Support/GOW3Lab/runs/gow3gpu_pm4_drain_20261023_200046_m2_kosmickrisp/`. The native renderer needs an SDL Cocoa window and `CAMetalLayer`; its macOS port creates those on the process main thread and pumps SDL events on the main dispatch queue. SDL's own Vulkan window path is not used; the pinned renderer creates `VK_EXT_metal_surface` from the SDL Metal layer.

The candidate C ABI has no shutdown function. The smoke process exits after the candidate queue drains because the window timer and kernel service still lack coordinated teardown. The first failed initialization attempts and their logs remain in the earlier `gow3gpu_init_20261023_m2_kosmickrisp` run folder.
