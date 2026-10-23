#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "host_runtime_test_support.h"

namespace {
using RegisterKernel = void (*)();
using Resolve = uintptr_t (*)(const char*);
using SymbolCount = unsigned (*)();

template <typename T>
T Load(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gow3gpu-abi-smoke /path/to/libgow3gpu.dylib\n");
        return 2;
    }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    const auto register_kernel = Load<RegisterKernel>(library, "gow3gpu_register_kernel");
    const auto resolve = Load<Resolve>(library, "gow3gpu_resolve");
    const auto symbol_count = Load<SymbolCount>(library, "gow3gpu_symbol_count");
    if (!register_kernel || !resolve || !symbol_count) {
        std::fprintf(stderr, "required C ABI export missing\n");
        dlclose(library);
        return 1;
    }
    if (!Gow3GpuHostRuntimeTest::Install(library)) {
        std::fprintf(stderr, "host runtime API install failed\n");
        return 1;
    }

    register_kernel();
    const unsigned count = symbol_count();
    const uintptr_t create_equeue = resolve("D0OdFMjp46I#libkernel#libkernel");
    const uintptr_t missing = resolve("NOT_A_REAL_GOW3_NID#libkernel#libkernel");
    const bool passed = count > 0 && create_equeue != 0 && missing == 0;
    std::printf("{\"abi_load\":true,\"kernel_symbols\":%u,\"create_equeue_resolved\":%s,\"unknown_nid_returns_zero\":%s,\"verdict\":\"%s\"}\n",
                count, create_equeue ? "true" : "false", missing == 0 ? "true" : "false",
                passed ? "PASS" : "FAIL");
    std::fflush(stdout);
    // register_kernel starts the imported kernel-service thread; there is no
    // shutdown API yet, so only this short-lived smoke process exits here.
    std::_Exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
