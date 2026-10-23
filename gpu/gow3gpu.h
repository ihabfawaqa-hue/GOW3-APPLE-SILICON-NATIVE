// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * God of War III Remastered (CUSA01623) GPU Subsystem Interface Bridge.
 * Adapted from shadPS4 and Sohebdsa/gow3_pc (SpartanPort).
 * Provides a minimal, clean C interface bridging native ARM64 CPU AOT
 * runtime execution with the GNM / Vulkan / Metal graphics cores.
 */

#ifndef GOW3GPU_H
#define GOW3GPU_H

#include "recomp_runtime.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration for initializing the Gow3 GPU subsystem
typedef struct Gow3GpuConfig {
    uint32_t screen_width;
    uint32_t screen_height;
    bool enable_validation_layers;
    bool enable_pipeline_binary_cache;
    uint32_t staging_pool_mb;        // Default: 512 MB
    uint32_t fault_window_kb;        // Default: 256 KB
    void *host_mem_base;             // Guest memory base address on host
    uint64_t guest_dmem_size;        // Direct memory pool size (>= 5056 MB)
    void (*flip_callback)(void *user_data);
    void *callback_user_data;
} Gow3GpuConfig;

// Subsystem Lifecycle
int gow3gpu_init(const Gow3GpuConfig *config);
void gow3gpu_destroy(void);
bool gow3gpu_is_initialized(void);
void gow3gpu_wait_idle(void);

// Command Buffer Submission
int32_t gow3gpu_submit_command_buffers(GuestContext *ctx, uint32_t count,
                                      const uint64_t *dcb_gpu_addrs, const uint32_t *dcb_sizes,
                                      const uint64_t *ccb_gpu_addrs, const uint32_t *ccb_sizes);

// Frame Presentation
int gow3gpu_present(GuestContext *ctx, uint64_t addr, uint32_t width, uint32_t height,
                    uint32_t pitch, uint32_t tiling_mode, uint32_t pixel_format,
                    void (*on_done)(void *), void *arg);

// Memory & Page-Fault Tracking Synchronization
void gow3gpu_notify_memory_map(uint64_t gpu_addr, void *host_addr, uint64_t size, uint32_t flags);
void gow3gpu_notify_memory_unmap(uint64_t gpu_addr, uint64_t size);
void gow3gpu_notify_page_fault(uint64_t fault_addr);

// Symbol & NID Resolution
void* gow3gpu_resolve_nid(uint32_t nid);

// Telemetry & Diagnostics
uint64_t gow3gpu_get_commands_decoded_count(void);
uint64_t gow3gpu_get_draws_encoded_count(void);
uint64_t gow3gpu_get_pipelines_created_count(void);
uint64_t gow3gpu_get_textures_resolved_count(void);

#ifdef __cplusplus
}
#endif

#endif // GOW3GPU_H
