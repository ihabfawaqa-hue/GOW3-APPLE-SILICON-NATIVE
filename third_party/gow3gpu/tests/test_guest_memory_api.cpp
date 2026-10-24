// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3gpu.h"
#include "core/memory.h"
#include <xxhash.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
struct FakeMemory {
    std::array<std::uint8_t, 16> bytes{};
    std::uint32_t wait_value{};
    std::uint32_t condition{};
    std::uint64_t semaphore{};
    bool fail_read{};
    unsigned read_calls{};
    unsigned write_calls{};
    unsigned protect_calls{};
    unsigned hook_install_calls{};
    std::uint64_t last_write_address{};
    Gow3GpuGuestRangeHook map_hook{};
    Gow3GpuGuestRangeHook unmap_hook{};
    Gow3GpuGuestRangeHook invalidate_hook{};
    void* hook_context{};
    unsigned host_to_guest_calls{};
    unsigned atomic_calls{};
    unsigned allocate_calls{};
    unsigned release_calls{};
};

bool Region(FakeMemory& memory, std::uint64_t address, std::uint64_t* start,
            std::uint64_t* end, int* mapped) {
    (void)memory;
    if (address >= 0x1000 && address < 0x1004) {
        *start = 0x1000; *end = 0x1004; *mapped = 1; return true;
    }
    if (address >= 0x1004 && address < 0x1008) {
        *start = 0x1004; *end = 0x1008; *mapped = 0; return true;
    }
    if (address >= 0x1008 && address < 0x1010) {
        *start = 0x1008; *end = 0x1010; *mapped = 1; return true;
    }
    return false;
}

int IsMapped(void* context, std::uint64_t address, std::uint64_t size) {
    auto& memory = *static_cast<FakeMemory*>(context);
    std::uint64_t start{}, end{};
    int mapped{};
    return Region(memory, address, &start, &end, &mapped) && mapped &&
           size <= end - address;
}

int HostToGuest(void* context, const void* host_pointer, std::uint64_t* guest_address) {
    auto& memory = *static_cast<FakeMemory*>(context);
    ++memory.host_to_guest_calls;
    const auto host = reinterpret_cast<std::uintptr_t>(host_pointer);
    const auto start = reinterpret_cast<std::uintptr_t>(memory.bytes.data());
    if (!guest_address || host < start || host - start >= memory.bytes.size()) return 0;
    *guest_address = 0x1000 + (host - start);
    return 1;
}

int QueryRegion(void* context, std::uint64_t address, std::uint64_t* start,
                std::uint64_t* end, int* mapped) {
    return Region(*static_cast<FakeMemory*>(context), address, start, end, mapped);
}

int Read(void* context, std::uint64_t address, void* destination,
         std::uint64_t size) {
    auto& memory = *static_cast<FakeMemory*>(context);
    ++memory.read_calls;
    if (memory.fail_read || !IsMapped(context, address, size) || !destination) return 0;
    std::memcpy(destination, memory.bytes.data() + (address - 0x1000), size);
    return 1;
}

int AtomicLoad(void* context, std::uint64_t address, std::uint32_t size,
               std::uint64_t* value) {
    auto& memory = *static_cast<FakeMemory*>(context);
    ++memory.atomic_calls;
    if (!value) return 0;
    if (address == 0x2000 && size == sizeof(memory.wait_value)) {
        *value = __atomic_load_n(&memory.wait_value, __ATOMIC_ACQUIRE);
        return 1;
    }
    if (address == 0x2004 && size == sizeof(memory.condition)) {
        *value = __atomic_load_n(&memory.condition, __ATOMIC_ACQUIRE);
        return 1;
    }
    if (address == 0x2008 && size == sizeof(memory.semaphore)) {
        *value = __atomic_load_n(&memory.semaphore, __ATOMIC_ACQUIRE);
        return 1;
    }
    return 0;
}

int AtomicU64(void* context, std::uint64_t address, std::uint32_t operation,
              std::uint64_t operand, std::uint64_t* previous_value) {
    auto& memory = *static_cast<FakeMemory*>(context);
    ++memory.atomic_calls;
    if (address != 0x2008 || !previous_value) return 0;
    switch (operation) {
    case GOW3GPU_ATOMIC_FETCH_ADD:
        *previous_value = __atomic_fetch_add(&memory.semaphore, operand, __ATOMIC_ACQ_REL);
        return 1;
    case GOW3GPU_ATOMIC_FETCH_SUB:
        *previous_value = __atomic_fetch_sub(&memory.semaphore, operand, __ATOMIC_ACQ_REL);
        return 1;
    case GOW3GPU_ATOMIC_FETCH_SUB_IF_NONZERO: {
        std::uint64_t current = __atomic_load_n(&memory.semaphore, __ATOMIC_ACQUIRE);
        while (current >= operand) {
            if (__atomic_compare_exchange_n(&memory.semaphore, &current, current - operand,
                                            true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                *previous_value = current;
                return 1;
            }
        }
        *previous_value = current;
        return 1;
    }
    case GOW3GPU_ATOMIC_STORE_RELEASE:
        *previous_value = 0;
        __atomic_store_n(&memory.semaphore, operand, __ATOMIC_RELEASE);
        return 1;
    default:
        return 0;
    }
}

int Writeback(void* context, std::uint64_t address, const void* source,
              std::uint64_t size) {
    auto& memory = *static_cast<FakeMemory*>(context);
    ++memory.write_calls;
    if (!IsMapped(context, address, size) || !source) return 0;
    memory.last_write_address = address;
    std::memcpy(memory.bytes.data() + (address - 0x1000), source, size);
    return 1;
}

std::uint64_t ClampRange(void* context, std::uint64_t address,
                         std::uint64_t requested) {
    auto& memory = *static_cast<FakeMemory*>(context);
    std::uint64_t start{}, end{};
    int mapped{};
    if (!Region(memory, address, &start, &end, &mapped) || !mapped) return 0;
    return requested < end - address ? requested : end - address;
}

int Protect(void* context, std::uint64_t address, std::uint64_t size,
            int read, int write) {
    auto& memory = *static_cast<FakeMemory*>(context);
    assert(address == 0x1008 && size == 4096 && read == 1 && write == 1);
    ++memory.protect_calls;
    return 1;
}

int InstallHooks(void* context, Gow3GpuGuestRangeHook map,
                 Gow3GpuGuestRangeHook unmap,
                 Gow3GpuGuestRangeHook invalidate, void* hook_context) {
    auto& memory = *static_cast<FakeMemory*>(context);
    assert(map && unmap && invalidate);
    ++memory.hook_install_calls;
    memory.map_hook = map;
    memory.unmap_hook = unmap;
    memory.invalidate_hook = invalidate;
    memory.hook_context = hook_context;
    return 1;
}

int AllocateProcessMemory(void* context, std::uint64_t size,
                          std::uint64_t alignment, const char*,
                          std::uint64_t* guest, void** host) {
    auto& memory = *static_cast<FakeMemory*>(context);
    ++memory.allocate_calls;
    if (size != 8 || alignment != 8 || !guest || !host) return 0;
    *guest = 0x1008;
    *host = memory.bytes.data() + 8;
    return 1;
}

int ReleaseProcessMemory(void* context, std::uint64_t guest,
                         std::uint64_t size, void* host) {
    auto& memory = *static_cast<FakeMemory*>(context);
    ++memory.release_calls;
    return guest == 0x1008 && size == 8 && host == memory.bytes.data() + 8;
}
} // namespace

int main() {
    assert(gow3gpu_set_guest_memory_api(nullptr) == -1);

    FakeMemory fake;
    for (std::size_t i = 0; i < fake.bytes.size(); ++i) {
        fake.bytes[i] = static_cast<std::uint8_t>(0x40 + i);
    }
    Gow3GpuGuestMemoryApi api{
        .abi_version = GOW3GPU_GUEST_MEMORY_ABI_VERSION,
        .struct_size = sizeof(Gow3GpuGuestMemoryApi),
        .user_context = &fake,
        .is_mapped = IsMapped,
        .host_to_guest = HostToGuest,
        .query_region = QueryRegion,
        .read = Read,
        .atomic_load = AtomicLoad,
        .atomic_u64 = AtomicU64,
        .writeback = Writeback,
        .clamp_range_size = ClampRange,
        .gpu_protect = Protect,
        .install_gpu_hooks = InstallHooks,
        .allocate_process_memory = AllocateProcessMemory,
        .release_process_memory = ReleaseProcessMemory,
    };
    auto incomplete_api = api;
    incomplete_api.host_to_guest = nullptr;
    assert(gow3gpu_set_guest_memory_api(&incomplete_api) == -1);
    incomplete_api = api;
    incomplete_api.atomic_u64 = nullptr;
    assert(gow3gpu_set_guest_memory_api(&incomplete_api) == -1);
    incomplete_api = api;
    incomplete_api.allocate_process_memory = nullptr;
    assert(gow3gpu_set_guest_memory_api(&incomplete_api) == -1);
    assert(gow3gpu_set_guest_memory_api(&api) == 0);
    assert(gow3gpu_set_guest_memory_api(&api) == -2);

    auto* memory = Core::Memory::Instance();
    assert(memory->IsValidGpuMapping(0x1000, 4));
    assert(!memory->IsValidGpuMapping(0x1000, 8));
    assert(memory->ClampRangeSize(0x1000, 8) == 4);
    VAddr guest_address{};
    assert(memory->GuestAddressFromHost(fake.bytes.data() + 6, guest_address));
    assert(guest_address == 0x1006 && fake.host_to_guest_calls == 1);
    assert(!memory->GuestAddressFromHost(nullptr, guest_address));
    void* guest_host_pointer{};
    assert(memory->AllocateProcessMemory(8, 8, "test-labels", guest_address,
                                         guest_host_pointer));
    assert(guest_address == 0x1008 && guest_host_pointer == fake.bytes.data() + 8 &&
           fake.allocate_calls == 1);
    assert(memory->ReleaseProcessMemory(guest_address, 8, guest_host_pointer) &&
           fake.release_calls == 1);
    assert(!memory->AllocateProcessMemory(8, 3, "invalid-alignment", guest_address,
                                          guest_host_pointer));
    std::uint64_t atomic_value{};
    fake.wait_value = 0x12345678;
    assert(memory->AtomicLoad(0x2000, 4, atomic_value));
    assert(atomic_value == 0x12345678);
    fake.condition = 1;
    assert(memory->AtomicLoad(0x2004, 4, atomic_value) && atomic_value == 1);
    fake.semaphore = 2;
    std::uint64_t previous_value{};
    assert(memory->AtomicSemaphore(0x2008, GOW3GPU_ATOMIC_FETCH_ADD, 3,
                                   previous_value));
    assert(previous_value == 2 && fake.semaphore == 5);
    assert(memory->AtomicSemaphore(0x2008, GOW3GPU_ATOMIC_FETCH_SUB_IF_NONZERO, 1,
                                   previous_value));
    assert(previous_value == 5 && fake.semaphore == 4);
    assert(memory->AtomicSemaphore(0x2008, GOW3GPU_ATOMIC_FETCH_SUB_IF_NONZERO, 5,
                                   previous_value));
    assert(previous_value == 4 && fake.semaphore == 4);
    assert(memory->AtomicSemaphore(0x2008, GOW3GPU_ATOMIC_STORE_RELEASE, 9,
                                   previous_value));
    assert(fake.semaphore == 9);
    assert(memory->AtomicLoad(0x2008, 8, atomic_value) && atomic_value == 9);

    std::array<std::uint8_t, 12> copied;
    copied.fill(0xff);
    assert(memory->CopySparseMemory(0x1000, copied.data(), copied.size()));
    assert((copied == std::array<std::uint8_t, 12>{
        0x40, 0x41, 0x42, 0x43, 0, 0, 0, 0, 0x48, 0x49, 0x4a, 0x4b}));
    assert(fake.read_calls == 2);
    std::uint64_t sparse_hash{};
    assert(memory->HashSparseMemory(0x1000, copied.size(), 0, sparse_hash));
    assert(sparse_hash == XXH3_64bits(copied.data(), copied.size()));
    assert(fake.read_calls == 4);

    memory->GetAddressSpace().Protect(0x1008, 4096, Core::MemoryPermission::ReadWrite);
    assert(fake.protect_calls == 1);
    memory->SetRasterizer(nullptr);
    assert(fake.hook_install_calls == 0);
    auto* fake_rasterizer = reinterpret_cast<Vulkan::Rasterizer*>(std::uintptr_t{1});
    memory->SetRasterizer(fake_rasterizer);
    assert(fake.hook_install_calls == 1);
    assert(fake.map_hook && fake.unmap_hook && fake.invalidate_hook);

    const std::array<std::uint8_t, 2> replacement{0xa5, 0x5a};
    assert(memory->TryWriteBacking(0x1009, replacement.data(), replacement.size()));
    assert(fake.write_calls == 1 && fake.last_write_address == 0x1009);
    assert(fake.bytes[9] == 0xa5 && fake.bytes[10] == 0x5a);

    copied.fill(0xff);
    assert(!memory->CopySparseMemory(UINT64_MAX - 1, copied.data(), 4));
    assert((std::array<std::uint8_t, 4>{copied[0], copied[1], copied[2], copied[3]} ==
            std::array<std::uint8_t, 4>{0, 0, 0, 0}));

    fake.fail_read = true;
    copied.fill(0xff);
    assert(!memory->CopySparseMemory(0x1000, copied.data(), 4));
    assert((copied == std::array<std::uint8_t, 12>{0, 0, 0, 0,
                                                   0xff, 0xff, 0xff, 0xff,
                                                   0xff, 0xff, 0xff, 0xff}));

    return 0;
}
