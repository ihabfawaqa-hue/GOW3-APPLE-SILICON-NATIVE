// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask where available), and speculative helpers run at background priority so
// they do not take scheduling time from the guest.

#pragma once

#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <bit>
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace Gow3Threads {
#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

/// The calling thread is on a frame's critical path.
inline void MakeCritical() {}

/// The calling thread feeds the critical path ahead of time and must keep up with it.
inline void MakeSupporting() {}
#elif defined(__APPLE__)

/// Hardware threads available to the process.
inline unsigned Available() {
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Keep speculative worker activity below interactive guest threads.
inline void MakeBackground() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
}

/// The calling thread is on a frame's critical path (GPU command processing, recording, the
/// copies submissions wait for): performance cores. Set explicitly: a thread otherwise inherits
/// the class of whichever thread created it, so a pool started from a background helper ran on
/// the efficiency cores and submissions waited 10-30 ms a frame for its copies.
inline void MakeCritical() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

/// The calling thread feeds the critical path ahead of time and must keep up with it (the draw
/// scanner: a buffer it has not scanned when the GPU thread reaches it gets no prepared draws).
/// Below the critical threads, above the speculative helpers.
inline void MakeSupporting() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
}

#else

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}

/// The calling thread is on a frame's critical path.
inline void MakeCritical() {}

/// The calling thread feeds the critical path ahead of time and must keep up with it.
inline void MakeSupporting() {}
#endif

} // namespace Gow3Threads
