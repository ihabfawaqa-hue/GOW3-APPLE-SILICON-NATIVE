// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef GOW3GPU_RUNTIME_ADAPTER_H
#define GOW3GPU_RUNTIME_ADAPTER_H

#include "recomp_context.h"
#include "../../third_party/gow3gpu/gpu/gow3gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*Ps4Gow3GpuSetGuestMemoryApi)(const Gow3GpuGuestMemoryApi *api);
typedef int (*Ps4Gow3GpuSetHostRuntimeApi)(const Gow3GpuHostRuntimeApi *api);
typedef int (*Ps4Gow3GpuFaultHandler)(void *ucontext, void *address);

/* Installs the primary runtime's authoritative guest-memory and host-service
 * contracts before gow3gpu_init. The game still owns the only guest mapping. */
int ps4_gow3gpu_install_runtime_apis(GuestContext *ctx,
                                     Ps4Gow3GpuSetGuestMemoryApi set_memory_api,
                                     Ps4Gow3GpuSetHostRuntimeApi set_host_api,
                                     Ps4Gow3GpuFaultHandler fault_handler);

/* The runtime signal handler calls this before producing a fatal report. */
int ps4_gow3gpu_dispatch_fault(void *ucontext, void *address);

/* Guest VM lifecycle notifications used by recomp_vm_alloc/free and direct-memory remaps. */
void ps4_gow3gpu_guest_map(uint64_t address, uint64_t size);
void ps4_gow3gpu_guest_unmap(uint64_t address, uint64_t size);
void ps4_gow3gpu_guest_invalidate(uint64_t address, uint64_t size);

/* The candidate overlay can request a process restart. The application must consume this
 * request from its main loop and perform its normal shutdown/relaunch path. */
int ps4_gow3gpu_take_restart_request(void);

#ifdef __cplusplus
}
#endif

#endif
