#include "gow3gpu.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <chrono>
#include <array>

#include "host_runtime_test_support.h"

namespace {
using SetGuestMemoryApi = int (*)(const Gow3GpuGuestMemoryApi*);
using Init = int (*)(const Gow3GpuConfig*);
using RegisterKernel = void (*)();
using Resolve = uintptr_t (*)(const char*);
using SymbolCount = unsigned (*)();
using SubmitCommandBuffers = int32_t (*)(uint32_t, const uint32_t* const*, uint32_t*,
                                         const uint32_t* const*, uint32_t*);
using SubmitDone = int32_t (*)();
using WaitIdle = int (*)(void);

template <typename T>
T Load(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}

int IsMapped(void*, uint64_t, uint64_t) { return 0; }
int HostToGuest(void*, const void*, uint64_t*) { return 0; }
int QueryRegion(void*, uint64_t, uint64_t*, uint64_t*, int*) { return 0; }
int Read(void*, uint64_t, void*, uint64_t) { return 0; }
int AtomicLoad(void*, uint64_t, uint32_t, uint64_t*) { return 0; }
int AtomicU64(void*, uint64_t, uint32_t, uint64_t, uint64_t*) { return 0; }
int Writeback(void*, uint64_t, const void*, uint64_t) { return 0; }
uint64_t ClampRange(void*, uint64_t, uint64_t) { return 0; }
int GpuProtect(void*, uint64_t, uint64_t, int, int) { return 1; }
int InstallGpuHooks(void*, Gow3GpuGuestRangeHook map, Gow3GpuGuestRangeHook unmap,
                    Gow3GpuGuestRangeHook invalidate, void*) {
    return map && unmap && invalidate;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gow3gpu-renderer-init-smoke /path/to/libgow3gpu.dylib\n");
        return 2;
    }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    const auto set_memory_api = Load<SetGuestMemoryApi>(library, "gow3gpu_set_guest_memory_api");
    const auto init = Load<Init>(library, "gow3gpu_init");
    const auto register_kernel = Load<RegisterKernel>(library, "gow3gpu_register_kernel");
    const auto resolve = Load<Resolve>(library, "gow3gpu_resolve");
    const auto symbol_count = Load<SymbolCount>(library, "gow3gpu_symbol_count");
    const auto wait_idle = Load<WaitIdle>(library, "gow3gpu_wait_idle");
    if (!set_memory_api || !init || !register_kernel || !resolve || !symbol_count || !wait_idle) {
        std::fprintf(stderr, "required GoW3 GPU C ABI export missing\n");
        return 1;
    }

    const Gow3GpuGuestMemoryApi api{
        .abi_version = GOW3GPU_GUEST_MEMORY_ABI_VERSION,
        .struct_size = sizeof(Gow3GpuGuestMemoryApi),
        .user_context = nullptr,
        .is_mapped = IsMapped,
        .host_to_guest = HostToGuest,
        .query_region = QueryRegion,
        .read = Read,
        .atomic_load = AtomicLoad,
        .atomic_u64 = AtomicU64,
        .writeback = Writeback,
        .clamp_range_size = ClampRange,
        .gpu_protect = GpuProtect,
        .install_gpu_hooks = InstallGpuHooks,
    };
    if (set_memory_api(&api) != 0) {
        std::fprintf(stderr, "guest-memory API install failed\n");
        return 1;
    }
    if (!Gow3GpuHostRuntimeTest::Install(library)) {
        std::fprintf(stderr, "host runtime API install failed\n");
        return 1;
    }

    const Gow3GpuConfig config{
        .title = "GoW3 Vulkan renderer initialization smoke",
        .serial = "CUSA01623",
        .user_dir = "/tmp/gow3gpu-renderer-init-smoke",
        .sdk_version = 0,
        .psf_attributes = 0,
        .width = 640,
        .height = 360,
    };
    int init_result = -1;
    try {
        init_result = init(&config);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "renderer init threw: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "renderer init threw an unknown exception\n");
        return 1;
    }
    if (init_result != 0) {
        std::fprintf(stderr, "renderer init returned %d\n", init_result);
        return 1;
    }

    register_kernel();
    const unsigned count = symbol_count();
    const uintptr_t draw_index = resolve("HlTPoZ-oY7Y#libSceGnmDriver#libSceGnmDriver");
    const uintptr_t submit_address = resolve("zwY0YV91TTI#libSceGnmDriver#libSceGnmDriver");
    const uintptr_t submit_done_address = resolve("yvZ73uQUqrk#libSceGnmDriver#libSceGnmDriver");
    if (count == 0 || draw_index == 0 || submit_address == 0 || submit_done_address == 0) {
        std::fprintf(stderr, "renderer initialized but GNM registration was incomplete\n");
        return 1;
    }

    // This standalone probe verifies the real exported GNM HLE ABI and command
    // processor with a valid two-DWORD PM4 NOP packet. Keep the buffer alive
    // until submit-done has observed queue completion. It is a protocol smoke,
    // not a game workload and does not cross the GuestContext import shim.
    static std::array<uint32_t, 2> dcb{0xc0001000u, 0u};
    const uint32_t* dcb_addresses[] = {dcb.data()};
    uint32_t dcb_sizes[] = {static_cast<uint32_t>(dcb.size() * sizeof(uint32_t))};
    const auto submit = reinterpret_cast<SubmitCommandBuffers>(submit_address);
    const auto submit_done = reinterpret_cast<SubmitDone>(submit_done_address);
    const int32_t submit_result = submit(1, dcb_addresses, dcb_sizes, nullptr, nullptr);
    if (submit_result != 0) {
        std::fprintf(stderr, "candidate sceGnmSubmitCommandBuffers returned %d\n", submit_result);
        return 1;
    }
    const int32_t submit_done_result = submit_done();
    if (submit_done_result != 0) {
        std::fprintf(stderr, "candidate sceGnmSubmitDone returned %d\n", submit_done_result);
        return 1;
    }
    const int wait_idle_result = wait_idle();
    if (wait_idle_result != 0) {
        std::fprintf(stderr, "candidate queue drain returned %d\n", wait_idle_result);
        return 1;
    }

    std::printf("{\"guest_memory_contract\":\"synthetic_empty\",\"host_runtime_contract\":\"synthetic_callbacks\",\"renderer_init_returned\":true,\"gnm_symbols\":%u,\"draw_index_resolved\":true,\"synthetic_pm4_nop_submitted\":true,\"submit_result\":%d,\"submit_done_result\":%d,\"candidate_queue_drained\":true,\"guest_context_import_path\":false,\"driver_selected_by_loader\":true,\"teardown\":\"process_exit_only\",\"verdict\":\"PASS\"}\n", count, submit_result, submit_done_result);
    std::fflush(stdout);

    // This candidate API does not export a shutdown function, and its SDL
    // window thread is detached. End only this short-lived smoke process after
    // init has been observed; do not dlclose the library under that thread.
    std::_Exit(EXIT_SUCCESS);
}
