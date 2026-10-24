// gow3: the MemoryManager surface used by the video core and GnmDriver.
// Guest VAddrs are translated by the host memory service; they are not pointers.
#pragma once
#include "common/types.h"
#include "common/singleton.h"
#include "core/address_space.h"

namespace Vulkan { class Rasterizer; }

namespace Core {
class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);
    AddressSpace& GetAddressSpace() { return impl; }
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size);
    bool GuestAddressFromHost(const void* host_pointer, VAddr& guest_address);
    bool AllocateProcessMemory(u64 size, u64 alignment, const char* name,
                               VAddr& guest_address, void*& host_pointer);
    bool ReleaseProcessMemory(VAddr guest_address, u64 size, void* host_pointer);
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    bool CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool HashSparseMemory(VAddr source, u64 size, u64 seed, u64& hash);
    bool AtomicLoad(VAddr address, u32 size, u64& value);
    bool AtomicSemaphore(VAddr address, u32 operation, u64 operand, u64& previous_value);
    bool TryWriteBacking(VAddr guest_address, const void* data, u64 size);
    /// CPU wrote guest memory outside page tracking (e.g. decoded video frames).
    void InvalidateMemory(VAddr address, u64 size);
    Vulkan::Rasterizer* GetRasterizer() const { return rasterizer; }

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};
using Memory = Common::Singleton<MemoryManager>;
} // namespace Core
