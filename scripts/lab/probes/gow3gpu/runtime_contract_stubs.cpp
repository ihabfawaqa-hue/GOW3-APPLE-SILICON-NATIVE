// Test-only symbols used to load the standalone bridge and exercise NID registration.
// These deliberately do not model guest memory and must never be linked into ps4_app.
#include <csetjmp>
#include <cstddef>
#include <cstdint>

extern "C" {
uint64_t runtime_disabled_optimizations = 0;
__thread sigjmp_buf* runtime_fault_recover = nullptr;

int32_t* runtime_errno(void) {
    static thread_local int32_t value = 0;
    return &value;
}
int runtime_memory_is_mapped(uintptr_t, uint64_t) { return 0; }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { return 0; }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t size) { return size; }
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped) {
    if (start) *start = address;
    if (end) *end = address;
    if (mapped) *mapped = 0;
    return 0;
}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) {}
using RuntimeGpuRange = void (*)(uintptr_t, uint64_t);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange, RuntimeGpuRange, RuntimeGpuRange) {}
uint64_t runtime_process_time_us(void) { return 0; }
uint64_t runtime_process_time_counter(void) { return 0; }
uint64_t runtime_tsc_frequency(void) { return 1'000'000'000; }
void runtime_restart(void) {}
void runtime_thread_attach_host(const char*) {}
int runtime_file_translate(const char*, char*, size_t) { return -1; }
int runtime_fault_recover_stub(void) { return 0; }
}
