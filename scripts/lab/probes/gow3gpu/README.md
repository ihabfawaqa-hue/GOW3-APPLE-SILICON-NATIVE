# Standalone GoW3 GPU C ABI smoke

This smoke dynamically loads the native `gow3gpu` shared library, calls `gow3gpu_register_kernel()`, verifies a known imported NID resolves, and checks an unknown NID returns zero.

```sh
cmake -S scripts/lab/probes/gow3gpu -B /tmp/gow3_gpu_abi_smoke_build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gow3_gpu_abi_smoke_build -j4
/tmp/gow3_gpu_abi_smoke_build/gow3gpu-abi-smoke /path/to/libgow3gpu.dylib
```

`runtime_contract_stubs.cpp` exists only to satisfy `dlopen` for this isolated registration test. Its functions do not model guest memory, page faults, timing, or restart behavior and must never be linked into `ps4_app`.

This test does not call `gow3gpu_init`: the current init path constructs the Vulkan presenter during GNM registration, and the M2 fails required `robustBufferAccess2` and `nullDescriptor` features. The current candidate C ABI exposes no shutdown function; `gow3gpu_init` also detaches the window thread and the window-close path calls `_Exit(0)`. Therefore full renderer initialization and teardown remain untested and require a lifecycle patch plus a compatible device or a clean feature-failure path.
