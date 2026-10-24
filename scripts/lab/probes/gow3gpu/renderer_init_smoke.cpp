#include "gow3gpu.h"
#include "gow3gpu_gnm_adapter.h"

#include <dlfcn.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <chrono>
#include <array>
#include <cstring>

#include "host_runtime_test_support.h"

namespace {
struct GuestMemory {
    alignas(16) std::array<uint8_t, 0x50000> bytes{};
    uint64_t allocation_address{0x40000};
    uint64_t allocation_size{};
};

using SetGuestMemoryApi = int (*)(const Gow3GpuGuestMemoryApi*);
using Init = int (*)(const Gow3GpuConfig*);
using RegisterKernel = void (*)();
using Resolve = uintptr_t (*)(const char*);
using SymbolCount = unsigned (*)();
using SubmitCommandBuffers = int32_t (*)(uint32_t, const uint32_t* const*, uint32_t*,
                                         const uint32_t* const*, uint32_t*);
using SubmitDone = int32_t (*)();
using WaitIdle = int (*)(void);
using VideoOutOpen = int32_t (*)(int32_t, int32_t, int32_t, const void*);
using VideoOutGetBufferLabelAddress = int32_t (*)(int32_t, uintptr_t*);
using VideoOutClose = int32_t (*)(int32_t);
SubmitCommandBuffers g_candidate_submit = nullptr;

template <typename T>
T Load(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}

int IsMapped(void* context, uint64_t address, uint64_t size) {
    const auto* memory = static_cast<const GuestMemory*>(context);
    return memory && size != 0 && address >= 0x10000 &&
           address < memory->bytes.size() &&
           size <= memory->bytes.size() - address;
}
int HostToGuest(void* context, const void* host, uint64_t* guest) {
    const auto* memory = static_cast<const GuestMemory*>(context);
    if (!memory || !host || !guest) return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory->bytes.data());
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(host);
    if (pointer < base || pointer - base >= memory->bytes.size()) return 0;
    *guest = pointer - base;
    return *guest >= 0x10000;
}
int QueryRegion(void* context, uint64_t address, uint64_t* start,
                uint64_t* end, int* mapped) {
    const auto* memory = static_cast<const GuestMemory*>(context);
    if (!memory || !start || !end || !mapped ||
        address >= memory->bytes.size()) return 0;
    if (address < 0x10000) {
        *start = 0;
        *end = 0x10000;
        *mapped = 0;
    } else {
        *start = 0x10000;
        *end = memory->bytes.size();
        *mapped = 1;
    }
    return 1;
}
int Read(void* context, uint64_t address, void* destination, uint64_t size) {
    auto* memory = static_cast<GuestMemory*>(context);
    if (!destination || !IsMapped(context, address, size)) return 0;
    std::memcpy(destination, memory->bytes.data() + address,
                static_cast<size_t>(size));
    return 1;
}
int AtomicLoad(void* context, uint64_t address, uint32_t size, uint64_t* value) {
    auto* memory = static_cast<GuestMemory*>(context);
    if (!value || (size != 1 && size != 4 && size != 8) || address % size ||
        !IsMapped(context, address, size)) return 0;
    const void* pointer = memory->bytes.data() + address;
    if (size == 1) *value = __atomic_load_n(static_cast<const uint8_t*>(pointer), __ATOMIC_ACQUIRE);
    else if (size == 4) *value = __atomic_load_n(static_cast<const uint32_t*>(pointer), __ATOMIC_ACQUIRE);
    else *value = __atomic_load_n(static_cast<const uint64_t*>(pointer), __ATOMIC_ACQUIRE);
    return 1;
}
int AtomicU64(void* context, uint64_t address, uint32_t operation,
             uint64_t operand, uint64_t* previous) {
    auto* memory = static_cast<GuestMemory*>(context);
    if (!previous || (address & 7) || !IsMapped(context, address, 8)) return 0;
    auto* pointer = reinterpret_cast<uint64_t*>(memory->bytes.data() + address);
    if (operation == GOW3GPU_ATOMIC_FETCH_ADD)
        *previous = __atomic_fetch_add(pointer, operand, __ATOMIC_ACQ_REL);
    else if (operation == GOW3GPU_ATOMIC_FETCH_SUB)
        *previous = __atomic_fetch_sub(pointer, operand, __ATOMIC_ACQ_REL);
    else if (operation == GOW3GPU_ATOMIC_STORE_RELEASE)
        *previous = __atomic_exchange_n(pointer, operand, __ATOMIC_RELEASE);
    else if (operation == GOW3GPU_ATOMIC_FETCH_SUB_IF_NONZERO) {
        uint64_t current = __atomic_load_n(pointer, __ATOMIC_ACQUIRE);
        do {
            if (!current) {
                *previous = 0;
                return 1;
            }
        } while (!__atomic_compare_exchange_n(pointer, &current, current - operand,
                                               1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
        *previous = current;
    } else return 0;
    return 1;
}
int Writeback(void* context, uint64_t address, const void* source, uint64_t size) {
    auto* memory = static_cast<GuestMemory*>(context);
    if (!source || !IsMapped(context, address, size)) return 0;
    std::memmove(memory->bytes.data() + address, source,
                 static_cast<size_t>(size));
    return 1;
}
uint64_t ClampRange(void* context, uint64_t address, uint64_t requested) {
    const auto* memory = static_cast<const GuestMemory*>(context);
    if (!memory || address < 0x10000 || address >= memory->bytes.size()) return 0;
    const uint64_t available = memory->bytes.size() - address;
    return requested < available ? requested : available;
}
int GpuProtect(void* context, uint64_t address, uint64_t size, int, int) {
    return IsMapped(context, address, size);
}
int InstallGpuHooks(void*, Gow3GpuGuestRangeHook map, Gow3GpuGuestRangeHook unmap,
                    Gow3GpuGuestRangeHook invalidate, void*) {
    return map && unmap && invalidate;
}
int AllocateProcessMemory(void* context, uint64_t size, uint64_t alignment,
                          const char*, uint64_t* guest, void** host) {
    auto* memory = static_cast<GuestMemory*>(context);
    if (!memory || !guest || !host || !size || !alignment ||
        (alignment & (alignment - 1)) || memory->allocation_size ||
        (memory->allocation_address & (alignment - 1)) ||
        !IsMapped(context, memory->allocation_address, size)) return 0;
    memory->allocation_size = size;
    *guest = memory->allocation_address;
    *host = memory->bytes.data() + memory->allocation_address;
    return 1;
}
int ReleaseProcessMemory(void* context, uint64_t guest, uint64_t size,
                         void* host) {
    auto* memory = static_cast<GuestMemory*>(context);
    if (!memory || guest != memory->allocation_address ||
        size != memory->allocation_size ||
        host != memory->bytes.data() + memory->allocation_address) return 0;
    memory->allocation_size = 0;
    return 1;
}

int32_t VerifyAndSubmit(uint32_t count, const uint32_t** dcb_addresses,
                        uint32_t* dcb_sizes, const uint32_t** ccb_addresses,
                        uint32_t* ccb_sizes) {
    if (count != 1 || !dcb_addresses || !dcb_addresses[0] || !dcb_sizes ||
        dcb_sizes[0] != 8 || dcb_addresses[0][0] != 0xc0001000u ||
        dcb_addresses[0][1] != 0 || ccb_addresses || ccb_sizes) {
        std::fprintf(stderr, "marshalled PM4 payload mismatch: count=%u dcb=%p size=%u first=%08x second=%08x ccb=%p/%p\n",
                     count, dcb_addresses ? (const void*)dcb_addresses[0] : nullptr,
                     dcb_sizes ? dcb_sizes[0] : 0,
                     dcb_addresses && dcb_addresses[0] ? dcb_addresses[0][0] : 0,
                     dcb_addresses && dcb_addresses[0] ? dcb_addresses[0][1] : 0,
                     (const void*)ccb_addresses, (const void*)ccb_sizes);
        return -1;
    }
    return g_candidate_submit(count, dcb_addresses, dcb_sizes,
                              ccb_addresses, ccb_sizes);
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
        .allocate_process_memory = AllocateProcessMemory,
        .release_process_memory = ReleaseProcessMemory,
    };
    GuestMemory guest_memory{};
    Gow3GpuGuestMemoryApi memory_api = api;
    memory_api.user_context = &guest_memory;
    if (set_memory_api(&memory_api) != 0) {
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
    const uintptr_t videoout_open_address =
        resolve("Up36PTk687E#libSceVideoOut#libSceVideoOut");
    const uintptr_t videoout_label_address =
        resolve("OcQybQejHEY#libSceVideoOut#libSceVideoOut");
    const uintptr_t videoout_close_address =
        resolve("uquVH4-Du78#libSceVideoOut#libSceVideoOut");
    if (count == 0 || draw_index == 0 || submit_address == 0 ||
        submit_done_address == 0 || videoout_open_address == 0 ||
        videoout_label_address == 0 || videoout_close_address == 0) {
        std::fprintf(stderr, "renderer initialized but GNM registration was incomplete\n");
        return 1;
    }

    // Cross the same GuestContext register ABI as the primary import shim, then
    // dispatch through the candidate's real HLE pointer. The adapter owns a
    // checked host copy until candidate submit-done has drained Liverpool.
    static constexpr uint64_t dcb_array_va = 0x10000;
    static constexpr uint64_t dcb_sizes_va = 0x10020;
    static constexpr uint64_t dcb_buffer_va = 0x20000;
    static constexpr std::array<uint32_t, 2> nop{0xc0001000u, 0u};
    const uint64_t dcb_addresses[] = {dcb_buffer_va};
    const uint32_t dcb_sizes[] = {static_cast<uint32_t>(nop.size() * sizeof(uint32_t))};
    std::memcpy(guest_memory.bytes.data() + dcb_array_va,
                dcb_addresses, sizeof(dcb_addresses));
    std::memcpy(guest_memory.bytes.data() + dcb_sizes_va,
                dcb_sizes, sizeof(dcb_sizes));
    std::memcpy(guest_memory.bytes.data() + dcb_buffer_va, nop.data(), sizeof(nop));

    GuestContext guest_context{};
    guest_context.mem_base = guest_memory.bytes.data();
    guest_context.mem_size = guest_memory.bytes.size();
    guest_context.rdi = 1;
    guest_context.rsi = dcb_array_va;
    guest_context.rdx = dcb_sizes_va;
    pthread_mutex_init(&guest_context.vm_mutex, nullptr);
    g_candidate_submit = reinterpret_cast<SubmitCommandBuffers>(submit_address);
    const auto submit = reinterpret_cast<Ps4Gow3GpuSubmitCommandBuffersFn>(VerifyAndSubmit);
    const auto submit_done = reinterpret_cast<Ps4Gow3GpuSubmitDoneFn>(submit_done_address);
    const int32_t submit_result =
        ps4_gow3gpu_submit_from_context(&guest_context, &memory_api, submit);
    if (submit_result != 0) {
        std::fprintf(stderr, "candidate sceGnmSubmitCommandBuffers returned %d\n", submit_result);
        return 1;
    }
    std::memset(guest_memory.bytes.data() + dcb_buffer_va, 0, sizeof(nop));
    if (ps4_gow3gpu_retained_command_bytes() != sizeof(nop)) {
        std::fprintf(stderr, "GuestContext adapter did not retain its PM4 copy\n");
        return 1;
    }
    const int32_t submit_done_result = ps4_gow3gpu_submit_done(submit_done, wait_idle);
    if (submit_done_result != 0) {
        std::fprintf(stderr, "candidate sceGnmSubmitDone returned %d\n", submit_done_result);
        return 1;
    }
    pthread_mutex_destroy(&guest_context.vm_mutex);
    const int wait_idle_result = wait_idle();
    if (wait_idle_result != 0) {
        std::fprintf(stderr, "candidate queue drain returned %d\n", wait_idle_result);
        return 1;
    }

    const auto videoout_open = reinterpret_cast<VideoOutOpen>(videoout_open_address);
    const auto videoout_get_label = reinterpret_cast<VideoOutGetBufferLabelAddress>(
        videoout_label_address);
    const auto videoout_close = reinterpret_cast<VideoOutClose>(videoout_close_address);
    const int32_t videoout_handle = videoout_open(0, 0, 0, nullptr);
    uintptr_t label_guest_address = 0;
    uint64_t translated_label_address = 0;
    if (videoout_handle != 1 ||
        videoout_get_label(videoout_handle, &label_guest_address) != 16 ||
        label_guest_address != guest_memory.allocation_address ||
        !HostToGuest(&guest_memory,
                     guest_memory.bytes.data() + label_guest_address,
                     &translated_label_address) ||
        translated_label_address != label_guest_address) {
        std::fprintf(stderr,
                     "VideoOut label import did not return the allocated guest VA\n");
        return 1;
    }
    auto* videoout_labels = reinterpret_cast<uint64_t*>(
        guest_memory.bytes.data() + label_guest_address);
    videoout_labels[2] = 0x1122334455667788ULL;
    uint64_t observed_label = 0;
    if (!memory_api.atomic_load(memory_api.user_context,
                                label_guest_address + 2 * sizeof(uint64_t),
                                sizeof(uint64_t), &observed_label) ||
        observed_label != videoout_labels[2] ||
        videoout_close(videoout_handle) != 0) {
        std::fprintf(stderr,
                     "VideoOut label guest VA and host view did not alias\n");
        return 1;
    }

    std::printf("{\"guest_memory_contract\":\"synthetic_mapped\",\"host_runtime_contract\":\"synthetic_callbacks\",\"renderer_init_returned\":true,\"gnm_symbols\":%u,\"draw_index_resolved\":true,\"synthetic_pm4_nop_submitted\":true,\"submit_result\":%d,\"submit_done_result\":%d,\"candidate_queue_drained\":true,\"guest_context_marshaller\":true,\"videoout_label_guest_va\":%llu,\"host_guest_label_alias\":true,\"real_game_import_path\":false,\"driver_selected_by_loader\":true,\"teardown\":\"process_exit_only\",\"verdict\":\"PASS\"}\n", count, submit_result, submit_done_result, static_cast<unsigned long long>(label_guest_address));
    std::fflush(stdout);

    // This candidate API does not export a shutdown function, and its SDL
    // window thread is detached. End only this short-lived smoke process after
    // init has been observed; do not dlclose the library under that thread.
    std::_Exit(EXIT_SUCCESS);
}
