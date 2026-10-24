// SPDX-License-Identifier: GPL-2.0-or-later
// Host-owned runtime services for the imported GPU/HLE boundary.
#include "../gow3gpu.h"
#include "gow3_toggles.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {
std::mutex g_host_runtime_mutex;
Gow3GpuHostRuntimeApi g_host_runtime_api{};
bool g_host_runtime_api_installed{};
thread_local bool g_host_thread_attached{};
thread_local int32_t g_fallback_errno{};

bool GetHostRuntimeApi(Gow3GpuHostRuntimeApi& api) {
    std::scoped_lock lock{g_host_runtime_mutex};
    if (!g_host_runtime_api_installed) return false;
    api = g_host_runtime_api;
    return true;
}

[[noreturn]] void MissingRequiredHostCallback(const char* name) {
    std::fprintf(stderr, "[gow3gpu-runtime] required host callback unavailable: %s\n", name);
    std::fflush(stderr);
    std::abort();
}
} // namespace

extern "C" {
// These state variables belong to the imported renderer. The app can inspect or
// change optimization bits through the explicit GPU diagnostics API later; it
// must not provide duplicate globals from its own runtime.
std::uint64_t runtime_disabled_optimizations = 0;
__thread sigjmp_buf* runtime_fault_recover = nullptr;

int gow3gpu_set_host_runtime_api(const Gow3GpuHostRuntimeApi* api) {
    if (!api || api->abi_version != GOW3GPU_HOST_RUNTIME_ABI_VERSION ||
        api->struct_size < sizeof(Gow3GpuHostRuntimeApi) || !api->errno_address ||
        !api->process_time_us || !api->process_time_counter || !api->tsc_frequency ||
        !api->translate_path || !api->attach_host_thread || !api->detach_host_thread ||
        !api->request_restart || !api->notify_gnm_eop || !api->notify_videoout_flip) {
        return -1;
    }
    std::scoped_lock lock{g_host_runtime_mutex};
    if (g_host_runtime_api_installed) return -2;
    g_host_runtime_api = *api;
    g_host_runtime_api_installed = true;
    return 0;
}

int gow3gpu_host_runtime_api_is_installed(void) {
    std::scoped_lock lock{g_host_runtime_mutex};
    return g_host_runtime_api_installed ? 1 : 0;
}

void gow3gpu_notify_host_gnm_eop(void) {
    Gow3GpuHostRuntimeApi api{};
    if (!GetHostRuntimeApi(api)) MissingRequiredHostCallback("notify_gnm_eop");
    api.notify_gnm_eop(api.user_context);
}

void gow3gpu_notify_host_videoout_flip(int32_t handle, int32_t buffer_index,
                                       int64_t flip_arg) {
    Gow3GpuHostRuntimeApi api{};
    if (!GetHostRuntimeApi(api)) MissingRequiredHostCallback("notify_videoout_flip");
    api.notify_videoout_flip(api.user_context, handle, buffer_index, flip_arg);
}

int32_t* runtime_errno(void) {
    Gow3GpuHostRuntimeApi api{};
    if (!GetHostRuntimeApi(api)) return &g_fallback_errno;
    int32_t* address = api.errno_address(api.user_context);
    return address ? address : &g_fallback_errno;
}

uint64_t runtime_process_time_us(void) {
    Gow3GpuHostRuntimeApi api{};
    return GetHostRuntimeApi(api) ? api.process_time_us(api.user_context) : 0;
}

uint64_t runtime_process_time_counter(void) {
    Gow3GpuHostRuntimeApi api{};
    return GetHostRuntimeApi(api) ? api.process_time_counter(api.user_context) : 0;
}

uint64_t runtime_tsc_frequency(void) {
    Gow3GpuHostRuntimeApi api{};
    return GetHostRuntimeApi(api) ? api.tsc_frequency(api.user_context) : 0;
}

int runtime_file_translate(const char* guest_path, char* host_path, size_t host_path_size) {
    if (!guest_path || !host_path || host_path_size == 0) return -1;
    Gow3GpuHostRuntimeApi api{};
    if (!GetHostRuntimeApi(api)) return -1;
    return api.translate_path(api.user_context, guest_path, host_path,
                              static_cast<uint64_t>(host_path_size));
}

void runtime_thread_attach_host(const char* name) {
    if (g_host_thread_attached) return;
    Gow3GpuHostRuntimeApi api{};
    if (!GetHostRuntimeApi(api)) MissingRequiredHostCallback("attach_host_thread");
    if (api.attach_host_thread(api.user_context, name) != 0) {
        MissingRequiredHostCallback("attach_host_thread failed");
    }
    g_host_thread_attached = true;
}

void runtime_thread_detach_host(void) {
    if (!g_host_thread_attached) return;
    Gow3GpuHostRuntimeApi api{};
    if (!GetHostRuntimeApi(api)) MissingRequiredHostCallback("detach_host_thread");
    api.detach_host_thread(api.user_context);
    g_host_thread_attached = false;
}

void runtime_restart(void) {
    Gow3GpuHostRuntimeApi api{};
    if (!GetHostRuntimeApi(api)) MissingRequiredHostCallback("request_restart");
    api.request_restart(api.user_context);
}
} // extern "C"
