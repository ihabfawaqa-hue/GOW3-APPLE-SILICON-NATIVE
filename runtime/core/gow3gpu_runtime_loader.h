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

/* The overlay's list picker: begin returns 0 when it cannot be shown; poll returns -1
 * while open, -2 when cancelled (or unavailable), else the chosen index. */
int ps4_gow3gpu_choice_begin(const char *title, const char *const *items,
                             const char *const *images, int count, int focus);
int ps4_gow3gpu_choice_poll(void);

/* The overlay (its menu, text entry or list picker) has the keyboard and pad. */
int ps4_gow3gpu_overlay_captures_input(void);

#ifdef __cplusplus
}
#endif

#endif
