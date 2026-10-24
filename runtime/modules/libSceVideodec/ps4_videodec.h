// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PS4_VIDEODEC_H
#define PS4_VIDEODEC_H

#include "recomp_runtime.h"
#include <stdint.h>

typedef struct Ps4VideodecConfigInfo {
  uint64_t this_size;
  uint32_t codec_type;
  uint32_t profile;
  uint32_t max_level;
  int32_t max_frame_width;
  int32_t max_frame_height;
  int32_t max_dpb_frame_count;
  uint64_t videodec_flags;
} Ps4VideodecConfigInfo;

typedef struct Ps4VideodecResourceInfo {
  uint64_t this_size;
  uint64_t cpu_memory_size;
  uint64_t cpu_memory;
  uint64_t cpu_gpu_memory_size;
  uint64_t cpu_gpu_memory;
  uint64_t max_frame_buffer_size;
  uint32_t frame_buffer_alignment;
  uint32_t reserved;
} Ps4VideodecResourceInfo;

typedef struct Ps4VideodecCtrl {
  uint64_t this_size;
  uint64_t handle;
  uint64_t version;
} Ps4VideodecCtrl;

typedef struct Ps4VideodecFrameBuffer {
  uint64_t this_size;
  uint64_t frame_buffer;
  uint64_t frame_buffer_size;
} Ps4VideodecFrameBuffer;

typedef struct Ps4VideodecInputData {
  uint64_t this_size;
  uint64_t access_unit;
  uint64_t access_unit_size;
  uint64_t pts;
  uint64_t dts;
  uint64_t attached_data;
} Ps4VideodecInputData;

typedef struct Ps4VideodecAvcInfo {
  uint32_t num_units_in_tick;
  uint32_t time_scale;
  uint8_t fixed_frame_rate_flag;
  uint8_t aspect_ratio_idc;
  uint16_t sar_width;
  uint16_t sar_height;
  uint8_t colour_primaries;
  uint8_t transfer_characteristics;
  uint8_t matrix_coefficients;
  uint8_t video_full_range_flag;
  uint32_t frame_crop_left_offset;
  uint32_t frame_crop_right_offset;
  uint32_t frame_crop_top_offset;
  uint32_t frame_crop_bottom_offset;
} Ps4VideodecAvcInfo;

typedef union Ps4VideodecCodecInfo {
  uint8_t reserved[64];
  Ps4VideodecAvcInfo avc;
} Ps4VideodecCodecInfo;

typedef struct Ps4VideodecPictureInfo {
  uint64_t this_size;
  uint32_t is_valid;
  uint32_t codec_type;
  uint32_t frame_width;
  uint32_t frame_pitch;
  uint32_t frame_height;
  uint32_t is_error_pic;
  uint64_t pts;
  uint64_t attached_data;
  Ps4VideodecCodecInfo codec;
} Ps4VideodecPictureInfo;

int32_t ps4_videodec_compute_resource_info(
    const Ps4VideodecConfigInfo *config, Ps4VideodecResourceInfo *resources);
void shim_sceVideodecQueryResourceInfo(GuestContext *ctx);
void shim_sceVideodecCreateDecoder(GuestContext *ctx);
void shim_sceVideodecReset(GuestContext *ctx);
void shim_sceVideodecDecode(GuestContext *ctx);
void shim_sceVideodecFlush(GuestContext *ctx);
void shim_sceVideodecDeleteDecoder(GuestContext *ctx);

#endif
