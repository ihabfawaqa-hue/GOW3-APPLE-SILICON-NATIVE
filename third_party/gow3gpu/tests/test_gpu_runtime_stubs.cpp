// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
// Match the runtime's C TLS ABI so Darwin exports the data symbol to the
// dynamically linked GPU library used by the isolated API test.
__thread sigjmp_buf* runtime_fault_recover = nullptr;
uint32_t runtime_disabled_optimizations = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void runtime_thread_attach_host(const char*) {}
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
}

