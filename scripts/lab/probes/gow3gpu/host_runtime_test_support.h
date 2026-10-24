#ifndef GOW3GPU_HOST_RUNTIME_TEST_SUPPORT_H
#define GOW3GPU_HOST_RUNTIME_TEST_SUPPORT_H

#include "gow3gpu.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>

namespace Gow3GpuHostRuntimeTest {
inline int32_t* ErrnoAddress(void*) {
    static thread_local int32_t value{};
    return &value;
}
inline uint64_t ProcessTimeUs(void*) {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start).count();
}
inline uint64_t ProcessTimeCounter(void* context) { return ProcessTimeUs(context) * 1000; }
inline uint64_t TscFrequency(void*) { return 2'130'000'000; }
inline int TranslatePath(void*, const char*, char*, uint64_t) { return -1; }
inline int AttachHostThread(void*, const char*) { return 0; }
inline void DetachHostThread(void*) {}
inline void RequestRestart(void*) {}
inline void NotifyGnmEop(void*) {}
inline void NotifyVideoOutFlip(void*, int32_t, int32_t, int64_t) {}

inline Gow3GpuHostRuntimeApi MakeApi() {
    return Gow3GpuHostRuntimeApi{
        .abi_version = GOW3GPU_HOST_RUNTIME_ABI_VERSION,
        .struct_size = sizeof(Gow3GpuHostRuntimeApi),
        .user_context = nullptr,
        .errno_address = ErrnoAddress,
        .process_time_us = ProcessTimeUs,
        .process_time_counter = ProcessTimeCounter,
        .tsc_frequency = TscFrequency,
        .translate_path = TranslatePath,
        .attach_host_thread = AttachHostThread,
        .detach_host_thread = DetachHostThread,
        .request_restart = RequestRestart,
        .notify_gnm_eop = NotifyGnmEop,
        .notify_videoout_flip = NotifyVideoOutFlip,
    };
}
inline bool Install(void* library) {
    using SetApi = int (*)(const Gow3GpuHostRuntimeApi*);
    auto set_api = reinterpret_cast<SetApi>(dlsym(library, "gow3gpu_set_host_runtime_api"));
    if (!set_api) return false;
    auto api = MakeApi();
    return set_api(&api) == 0;
}
} // namespace Gow3GpuHostRuntimeTest

#endif
