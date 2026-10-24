// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef GOW3GPU_RUNTIME_LOADER_H
#define GOW3GPU_RUNTIME_LOADER_H

#include "recomp_context.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the opt-in candidate PM4 route when PS4_GOW3GPU_BACKEND is explicitly
 * set to "gow3gpu-vulkan". A missing or invalid candidate is a startup error;
 * the runtime never silently falls back after the user selects this route. */
int ps4_gow3gpu_start_requested_backend(GuestContext *ctx);

/* Resolve an imported candidate function after the opt-in backend is active. */
uintptr_t ps4_gow3gpu_resolve_candidate(const char *scoped_nid);

#ifdef __cplusplus
}
#endif

#endif
