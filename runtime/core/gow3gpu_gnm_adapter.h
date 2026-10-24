// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef GOW3GPU_GNM_ADAPTER_H
#define GOW3GPU_GNM_ADAPTER_H

#include "recomp_context.h"
#include "../../third_party/gow3gpu/gpu/gow3gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t (*Ps4Gow3GpuSubmitCommandBuffersFn)(
    uint32_t count, const uint32_t **dcb_gpu_addrs,
    uint32_t *dcb_sizes_in_bytes, const uint32_t **ccb_gpu_addrs,
    uint32_t *ccb_sizes_in_bytes);
typedef int32_t (*Ps4Gow3GpuSubmitDoneFn)(void);
typedef int32_t (*Ps4Gow3GpuWaitIdleFn)(void);

/* Install once after the candidate GPU and runtime callback tables are ready.
 * This opt-in route makes the pinned candidate the sole Liverpool PM4 submit
 * owner; guest command bytes are copied until its submit-done drain completes. */
int ps4_gow3gpu_install_submission_backend(
    const Gow3GpuGuestMemoryApi *memory_api,
    Ps4Gow3GpuSubmitCommandBuffersFn submit,
    Ps4Gow3GpuSubmitDoneFn submit_done,
    Ps4Gow3GpuWaitIdleFn wait_idle);
int ps4_gow3gpu_submission_backend_active(void);
int32_t ps4_gow3gpu_submit_guest_arrays(
    GuestContext *ctx, uint32_t count, const uint64_t *dcb_guest_addresses,
    const uint32_t *dcb_sizes, const uint64_t *ccb_guest_addresses,
    const uint32_t *ccb_sizes);
int32_t ps4_gow3gpu_backend_submit_done(void);

/* Marshal the real sceGnmSubmitCommandBuffers GuestContext register ABI into
 * the pinned GPU library ABI. Guest command bytes are copied into host-owned
 * storage and retained until submit-done confirms the candidate has drained. */
int32_t ps4_gow3gpu_submit_from_context(
    GuestContext *ctx, const Gow3GpuGuestMemoryApi *memory_api,
    Ps4Gow3GpuSubmitCommandBuffersFn submit);

/* Candidate submit-done marks the guest frame boundary. Wait for full Liverpool
 * draw-pipe/deferred-signal retirement before releasing retained PM4 storage. */
int32_t ps4_gow3gpu_submit_done(Ps4Gow3GpuSubmitDoneFn submit_done,
                                Ps4Gow3GpuWaitIdleFn wait_idle);
/* Like ps4_gow3gpu_submit_done without the GPU idle wait: the command buffer
 * copies of a frame are freed GOW3GPU_RETAINED_FRAMES SubmitDone calls later. */
int32_t ps4_gow3gpu_submit_done_pipelined(Ps4Gow3GpuSubmitDoneFn submit_done,
                                          Ps4Gow3GpuWaitIdleFn wait_idle);

/* Bounded diagnostic used by the isolated smoke and runtime observability. */
uint64_t ps4_gow3gpu_retained_command_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
