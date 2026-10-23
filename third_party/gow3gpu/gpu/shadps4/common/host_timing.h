// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>

#include "common/arch.h"
#include "common/types.h"

#if defined(ARCH_X86_64) && defined(_MSC_VER)
#include <intrin.h>
#elif defined(ARCH_X86_64)
#include <immintrin.h>
#endif

namespace Common {

/// Monotonic wall-clock ticks expressed in nanoseconds on every supported host.
inline u64 MonotonicNanoseconds() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// CPU hint for short producer/consumer spins; it does not provide synchronization.
inline void CpuRelax() noexcept {
#if defined(ARCH_X86_64) && defined(_MSC_VER)
    _mm_pause();
#elif defined(ARCH_X86_64)
    __builtin_ia32_pause();
#elif defined(ARCH_ARM64)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

} // namespace Common
