// SPDX-License-Identifier: GPL-2.0-or-later
// Built only with -DGOW3GPU_TEST_HOOKS=ON for the local bridge smoke.
#include "../gow3gpu.h"
#include "gow3_toggles.h"

extern "C" int gow3gpu_runtime_fault_recovery_self_test(void) {
    sigjmp_buf recovery{};
    const int recovered = sigsetjmp(recovery, 1);
    if (recovered == 0) {
        runtime_fault_recover = &recovery;
        (void)gow3gpu_handle_fault(nullptr, nullptr);
        runtime_fault_recover = nullptr;
        return -1;
    }
    return runtime_fault_recover == nullptr ? 0 : -2;
}
