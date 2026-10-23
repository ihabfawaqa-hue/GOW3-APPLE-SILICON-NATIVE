// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * God of War III Remastered (CUSA01623) Deterministic GPU Capture Facility.
 * Captures guest PM4 command streams, Liverpool register state, shader bytecode,
 * vertex/index buffers, and texture descriptors for isolated offline replay.
 */

#ifndef GOW3_CAPTURE_H
#define GOW3_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Gow3DrawCaptureRecord {
    uint64_t submission_id;
    uint32_t draw_index;
    uint32_t primitive_type;
    uint32_t index_count;
    uint32_t vertex_count;

    // Registers
    uint64_t cb_color0_base;
    uint32_t cb_color0_info;
    uint64_t vs_gpu_addr;
    uint64_t ps_gpu_addr;

    // Buffers
    uint64_t vb_gpu_addr;
    uint32_t vb_size;
    uint32_t vb_stride;

    uint64_t ib_gpu_addr;
    uint32_t ib_size;
    uint32_t index_type;

    // Texture
    uint64_t texture_gpu_addr;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_format;
} Gow3DrawCaptureRecord;

// Capture Control API
int gow3_capture_init(const char *capture_directory);
void gow3_capture_record_draw(const Gow3DrawCaptureRecord *record, const void *vb_data, const void *ib_data);
void gow3_capture_record_submission(uint64_t submission_id, uint32_t dcb_count, const uint32_t *dcb_words, uint32_t dcb_word_count);
void gow3_capture_flush(void);
void gow3_capture_close(void);
bool gow3_capture_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // GOW3_CAPTURE_H
