#include "gow3gpu.h"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct TestContext {
    int32_t error{};
    unsigned attach_count{};
    unsigned detach_count{};
    unsigned restart_count{};
    char attached_name[64]{};
};

using SetApi = int (*)(const Gow3GpuHostRuntimeApi*);
using ErrnoAddress = int32_t* (*)();
using TimeValue = uint64_t (*)();
using TranslatePath = int (*)(const char*, char*, size_t);
using AttachHostThread = void (*)(const char*);
using DetachHostThread = void (*)();
using RequestRestart = void (*)();
using HandleFault = int (*)(void*, void*);
using RecoverySelfTest = int (*)();

template <typename T>
T Load(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}

int32_t* GetErrno(void* context) { return &static_cast<TestContext*>(context)->error; }
uint64_t GetTimeUs(void*) { return 123456; }
uint64_t GetTimeCounter(void*) { return 654321; }
uint64_t GetTscFrequency(void*) { return 2'130'000'000; }
int Translate(void*, const char* guest, char* host, uint64_t size) {
    constexpr char expected[] = "/app0/test.bin";
    constexpr char translated[] = "/mounted/app0/test.bin";
    if (!guest || std::strcmp(guest, expected) != 0 || size < sizeof(translated)) return -1;
    std::memcpy(host, translated, sizeof(translated));
    return 0;
}
int Attach(void* context, const char* name) {
    auto* test = static_cast<TestContext*>(context);
    ++test->attach_count;
    std::snprintf(test->attached_name, sizeof(test->attached_name), "%s", name ? name : "");
    return 0;
}
void Detach(void* context) { ++static_cast<TestContext*>(context)->detach_count; }
void Restart(void* context) { ++static_cast<TestContext*>(context)->restart_count; }
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gow3gpu-host-runtime-smoke /path/to/libgow3gpu.dylib\n");
        return 2;
    }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    const auto set_api = Load<SetApi>(library, "gow3gpu_set_host_runtime_api");
    const auto errno_address = Load<ErrnoAddress>(library, "runtime_errno");
    const auto process_time_us = Load<TimeValue>(library, "runtime_process_time_us");
    const auto process_time_counter = Load<TimeValue>(library, "runtime_process_time_counter");
    const auto tsc_frequency = Load<TimeValue>(library, "runtime_tsc_frequency");
    const auto translate = Load<TranslatePath>(library, "runtime_file_translate");
    const auto attach = Load<AttachHostThread>(library, "runtime_thread_attach_host");
    const auto detach = Load<DetachHostThread>(library, "runtime_thread_detach_host");
    const auto restart = Load<RequestRestart>(library, "runtime_restart");
    const auto handle_fault = Load<HandleFault>(library, "gow3gpu_handle_fault");
    const auto recovery_self_test = Load<RecoverySelfTest>(
        library, "gow3gpu_runtime_fault_recovery_self_test");
    auto* disabled_optimizations = static_cast<uint64_t*>(dlsym(library, "runtime_disabled_optimizations"));
    if (!set_api || !errno_address || !process_time_us || !process_time_counter ||
        !tsc_frequency || !translate || !attach || !detach || !restart || !handle_fault ||
        !disabled_optimizations || !recovery_self_test) {
        std::fprintf(stderr, "required host-runtime bridge symbol missing\n");
        return 1;
    }
    if (handle_fault(nullptr, nullptr) != 0) {
        std::fprintf(stderr, "unarmed null fault should be ignored\n");
        return 1;
    }

    TestContext context{};
    Gow3GpuHostRuntimeApi api{
        .abi_version = GOW3GPU_HOST_RUNTIME_ABI_VERSION,
        .struct_size = sizeof(Gow3GpuHostRuntimeApi),
        .user_context = &context,
        .errno_address = GetErrno,
        .process_time_us = GetTimeUs,
        .process_time_counter = GetTimeCounter,
        .tsc_frequency = GetTscFrequency,
        .translate_path = Translate,
        .attach_host_thread = Attach,
        .detach_host_thread = Detach,
        .request_restart = Restart,
    };
    auto invalid_api = api;
    invalid_api.abi_version++;
    if (set_api(&invalid_api) != -1 || set_api(&api) != 0 || set_api(&api) != -2) {
        std::fprintf(stderr, "host runtime API validation or one-time install failed\n");
        return 1;
    }

    int32_t* error = errno_address();
    char host_path[64]{};
    if (error != &context.error || process_time_us() != 123456 ||
        process_time_counter() != 654321 || tsc_frequency() != 2'130'000'000 ||
        translate("/app0/test.bin", host_path, sizeof(host_path)) != 0 ||
        std::strcmp(host_path, "/mounted/app0/test.bin") != 0) {
        std::fprintf(stderr, "errno, time, or VFS callback forwarding failed\n");
        return 1;
    }
    *error = 77;
    attach("smoke-thread");
    detach();
    restart();
    if (context.error != 77 || context.attach_count != 1 || context.detach_count != 1 ||
        context.restart_count != 1 || std::strcmp(context.attached_name, "smoke-thread") != 0) {
        std::fprintf(stderr, "host thread or restart callback forwarding failed\n");
        return 1;
    }

    *disabled_optimizations = 0x1234;
    if (*disabled_optimizations != 0x1234) return 1;

    if (recovery_self_test() != 0) {
        std::fprintf(stderr, "recoverable GPU fault did not return through siglongjmp\n");
        return 1;
    }

    std::printf("{\"host_runtime_api\":\"PASS\",\"errno_time_vfs_threads_restart\":\"PASS\",\"fault_recovery\":\"PASS\",\"verdict\":\"PASS\"}\n");
    return 0;
}
