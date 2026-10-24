// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_videodec.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORBIS_VIDEODEC_ERROR_API_FAIL 0x80C10000
#define ORBIS_VIDEODEC_ERROR_CODEC_TYPE 0x80C10001
#define ORBIS_VIDEODEC_ERROR_STRUCT_SIZE 0x80C10002
#define ORBIS_VIDEODEC_ERROR_HANDLE 0x80C10003
#define ORBIS_VIDEODEC_ERROR_AU_SIZE 0x80C10009
#define ORBIS_VIDEODEC_ERROR_AU_POINTER 0x80C1000A
#define ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE 0x80C1000B
#define ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_POINTER 0x80C1000C
#define ORBIS_VIDEODEC_ERROR_CONFIG_INFO 0x80C1000E
#define ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER 0x80C1000F

#define PS4_VIDEODEC_CPU_MEMORY_SIZE (16ULL * 1024ULL * 1024ULL)
#define PS4_VIDEODEC_CPU_GPU_OVERHEAD (8ULL * 1024ULL * 1024ULL)
#define PS4_VIDEODEC_MAX_ACCESS_UNIT (16ULL * 1024ULL * 1024ULL)
#define PS4_VIDEODEC_MAX_FRAME_SIZE (64ULL * 1024ULL * 1024ULL)
#define PS4_VIDEODEC_MAX_WIDTH 4096U
#define PS4_VIDEODEC_MAX_HEIGHT 2160U
#define PS4_VIDEODEC_MAX_DECODERS 4
#define PS4_VIDEODEC_HANDLE_SLOT_BITS 8
#define PS4_VIDEODEC_HANDLE_SLOT_MASK ((1ULL << PS4_VIDEODEC_HANDLE_SLOT_BITS) - 1)
#define PS4_VIDEODEC_HANDLE_GENERATION_MASK ((1ULL << (64 - PS4_VIDEODEC_HANDLE_SLOT_BITS)) - 1)

_Static_assert(sizeof(Ps4VideodecConfigInfo) == 40,
               "PS4 Videodec config ABI size changed");
_Static_assert(sizeof(Ps4VideodecResourceInfo) == 56,
               "PS4 Videodec resource ABI size changed");
_Static_assert(offsetof(Ps4VideodecResourceInfo, frame_buffer_alignment) == 48,
               "PS4 Videodec resource ABI offsets changed");
_Static_assert(sizeof(Ps4VideodecCtrl) == 24, "PS4 Videodec control ABI changed");
_Static_assert(sizeof(Ps4VideodecFrameBuffer) == 24,
               "PS4 Videodec framebuffer ABI changed");
_Static_assert(sizeof(Ps4VideodecInputData) == 48,
               "PS4 Videodec input ABI changed");
_Static_assert(sizeof(Ps4VideodecPictureInfo) == 112,
               "PS4 Videodec picture ABI changed");

typedef struct Ps4VideodecInstance {
  AVCodecContext *codec;
  struct SwsContext *scaler;
  uint64_t generation;
  uint64_t max_frame_buffer_size;
  uint32_t max_width;
  uint32_t max_height;
  int logged_first_frame;
} Ps4VideodecInstance;

static Ps4VideodecInstance g_decoders[PS4_VIDEODEC_MAX_DECODERS];
static pthread_mutex_t g_decoder_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_ffmpeg_once = PTHREAD_ONCE_INIT;

static void ffmpeg_initialize(void) { av_log_set_level(AV_LOG_ERROR); }

static void shim_finish(GuestContext *ctx, int32_t result) {
  ctx->rax = (uint64_t)(int64_t)result;
  ctx->rsp += 8;
}

static int align_up_u64(uint64_t value, uint64_t alignment, uint64_t *out) {
  if (!out || alignment == 0 || value > UINT64_MAX - (alignment - 1))
    return 0;
  *out = ((value + alignment - 1) / alignment) * alignment;
  return 1;
}

static int align_up_u32(uint32_t value, uint32_t alignment, uint32_t *out) {
  if (!out || alignment == 0 || value > UINT32_MAX - (alignment - 1))
    return 0;
  *out = ((value + alignment - 1) / alignment) * alignment;
  return 1;
}

static uint64_t make_handle(size_t slot, uint64_t generation) {
  return ((generation & PS4_VIDEODEC_HANDLE_GENERATION_MASK)
          << PS4_VIDEODEC_HANDLE_SLOT_BITS) |
         (uint64_t)(slot + 1);
}

static Ps4VideodecInstance *find_instance_locked(uint64_t handle) {
  const uint64_t encoded_slot = handle & PS4_VIDEODEC_HANDLE_SLOT_MASK;
  const uint64_t generation = handle >> PS4_VIDEODEC_HANDLE_SLOT_BITS;
  if (encoded_slot == 0 || encoded_slot > PS4_VIDEODEC_MAX_DECODERS ||
      generation == 0)
    return NULL;

  Ps4VideodecInstance *instance = &g_decoders[encoded_slot - 1];
  if (!instance->codec || instance->generation != generation)
    return NULL;
  return instance;
}

static void destroy_instance_locked(Ps4VideodecInstance *instance) {
  if (!instance)
    return;
  avcodec_free_context(&instance->codec);
  sws_freeContext(instance->scaler);
  instance->scaler = NULL;
  instance->max_frame_buffer_size = 0;
  instance->max_width = 0;
  instance->max_height = 0;
  instance->logged_first_frame = 0;
}

int32_t ps4_videodec_compute_resource_info(
    const Ps4VideodecConfigInfo *config, Ps4VideodecResourceInfo *resources) {
  if (!config || !resources)
    return (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
  if (config->this_size != sizeof(*config))
    return (int32_t)ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;

  uint32_t width = 0;
  uint32_t height = 0;
  if (config->max_frame_width > 0 && config->max_frame_height > 0) {
    width = (uint32_t)config->max_frame_width;
    height = (uint32_t)config->max_frame_height;
  } else {
    width = 1920;
    height = 1080;
    if (config->max_level >= 150) {
      width = 3840;
      height = 2160;
    }
  }

  uint64_t aligned_width = 0;
  uint64_t aligned_height = 0;
  if (!align_up_u64(width, 256, &aligned_width) ||
      !align_up_u64(height, 16, &aligned_height) ||
      aligned_width > UINT64_MAX / aligned_height)
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;

  const uint64_t pixels = aligned_width * aligned_height;
  if (pixels > UINT64_MAX / 3)
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
  const uint64_t frame_size = (pixels * 3) / 2;

  uint64_t padded_frame_size = 0;
  if (!align_up_u64(frame_size, 256, &padded_frame_size) ||
      padded_frame_size > UINT64_MAX - 0x4000)
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
  padded_frame_size += 0x4000;

  const uint64_t dpb_count = config->max_dpb_frame_count > 0
                                 ? (uint64_t)config->max_dpb_frame_count
                                 : 8;
  const uint64_t surface_count = dpb_count + 2;
  if (padded_frame_size >
      (UINT64_MAX - PS4_VIDEODEC_CPU_GPU_OVERHEAD) / surface_count)
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;

  memset(resources, 0, sizeof(*resources));
  resources->this_size = sizeof(*resources);
  resources->cpu_memory_size = PS4_VIDEODEC_CPU_MEMORY_SIZE;
  resources->cpu_gpu_memory_size =
      padded_frame_size * surface_count + PS4_VIDEODEC_CPU_GPU_OVERHEAD;
  resources->max_frame_buffer_size = padded_frame_size;
  resources->frame_buffer_alignment = 0x100;
  return 0;
}

static int32_t convert_frame_to_nv12(Ps4VideodecInstance *instance,
                                     const AVFrame *frame,
                                     uint64_t fallback_pts,
                                     uint64_t attached_data,
                                     uint64_t framebuffer_size,
                                     uint8_t **pixels_out,
                                     size_t *pixels_size_out,
                                     Ps4VideodecPictureInfo *picture_out) {
  if (!instance || !frame || !pixels_out || !pixels_size_out || !picture_out ||
      frame->width <= 0 || frame->height <= 0 ||
      (uint32_t)frame->width > instance->max_width ||
      (uint32_t)frame->height > instance->max_height ||
      (frame->width & 1) || (frame->height & 1))
    return (int32_t)ORBIS_VIDEODEC_ERROR_CONFIG_INFO;

  uint32_t pitch = 0;
  uint32_t padded_height = 0;
  uint32_t padded_width = 0;
  if (!align_up_u32((uint32_t)frame->width, 64, &pitch) ||
      !align_up_u32((uint32_t)frame->height, 16, &padded_height) ||
      !align_up_u32((uint32_t)frame->width, 16, &padded_width))
    return (int32_t)ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE;

  const uint64_t y_size = (uint64_t)pitch * padded_height;
  if (y_size > UINT64_MAX / 3 * 2)
    return (int32_t)ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE;
  const uint64_t total_size = y_size + y_size / 2;
  if (total_size > framebuffer_size ||
      total_size > instance->max_frame_buffer_size ||
      total_size > PS4_VIDEODEC_MAX_FRAME_SIZE || total_size > SIZE_MAX)
    return (int32_t)ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE;

  uint8_t *pixels = (uint8_t *)calloc(1, (size_t)total_size);
  if (!pixels)
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
  uint8_t *dst[4] = {pixels, pixels + y_size, NULL, NULL};
  int dst_linesize[4] = {(int)pitch, (int)pitch, 0, 0};
  instance->scaler = sws_getCachedContext(
      instance->scaler, frame->width, frame->height,
      (enum AVPixelFormat)frame->format, frame->width, frame->height,
      AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
  if (!instance->scaler) {
    free(pixels);
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
  }

  const int rows = sws_scale(instance->scaler,
                             (const uint8_t *const *)frame->data,
                             frame->linesize, 0, frame->height, dst,
                             dst_linesize);
  if (rows != frame->height) {
    free(pixels);
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
  }

  memset(picture_out, 0, sizeof(*picture_out));
  picture_out->this_size = sizeof(*picture_out);
  picture_out->is_valid = 1;
  picture_out->frame_width = padded_width;
  picture_out->frame_pitch = pitch;
  picture_out->frame_height = padded_height;
  picture_out->pts = frame->pts != AV_NOPTS_VALUE
                         ? (uint64_t)frame->pts
                         : fallback_pts;
  picture_out->attached_data = attached_data;
  picture_out->codec.avc.colour_primaries =
      (uint8_t)frame->color_primaries;
  picture_out->codec.avc.transfer_characteristics =
      (uint8_t)frame->color_trc;
  picture_out->codec.avc.matrix_coefficients = (uint8_t)frame->colorspace;
  picture_out->codec.avc.video_full_range_flag =
      frame->color_range == AVCOL_RANGE_JPEG;
  picture_out->codec.avc.frame_crop_right_offset = pitch - frame->width;
  picture_out->codec.avc.frame_crop_bottom_offset = padded_height - frame->height;
  *pixels_out = pixels;
  *pixels_size_out = (size_t)total_size;
  return 0;
}

static int32_t write_picture(GuestContext *ctx, uint64_t picture_addr,
                             const Ps4VideodecFrameBuffer *framebuffer,
                             const Ps4VideodecPictureInfo *picture,
                             const uint8_t *pixels, size_t pixels_size) {
  if (pixels && pixels_size &&
      !recomp_guest_memory_write(ctx, framebuffer->frame_buffer, pixels,
                                 pixels_size))
    return (int32_t)ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_POINTER;
  if (!recomp_guest_memory_write(ctx, picture_addr, picture, sizeof(*picture)))
    return (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
  return 0;
}

void shim_sceVideodecQueryResourceInfo(GuestContext *ctx) {
  if (!ctx)
    return;

  if (!ctx->rdi || !ctx->rsi) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }

  Ps4VideodecConfigInfo config;
  uint64_t output_size = 0;
  if (!recomp_guest_memory_read(ctx, ctx->rdi, &config, sizeof(config)) ||
      !recomp_guest_memory_read(ctx, ctx->rsi, &output_size,
                                sizeof(output_size))) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }
  if (config.this_size != sizeof(config) ||
      output_size != sizeof(Ps4VideodecResourceInfo)) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_STRUCT_SIZE);
    return;
  }

  Ps4VideodecResourceInfo resources;
  const int32_t status = ps4_videodec_compute_resource_info(&config, &resources);
  if (status != 0) {
    shim_finish(ctx, status);
    return;
  }
  if (!recomp_guest_memory_write(ctx, ctx->rsi, &resources,
                                 sizeof(resources))) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }

  fprintf(stderr,
          "[videodec] QueryResourceInfo OK cfg=0x%llx out=0x%llx "
          "max=%dx%d level=%u dpb=%" PRIu64
          " cpu=%" PRIu64 " cpu_gpu=%" PRIu64 " frame=%" PRIu64 "\n",
          (unsigned long long)ctx->rdi, (unsigned long long)ctx->rsi,
          config.max_frame_width, config.max_frame_height, config.max_level,
          (uint64_t)(config.max_dpb_frame_count > 0
                         ? config.max_dpb_frame_count
                         : 8),
          resources.cpu_memory_size, resources.cpu_gpu_memory_size,
          resources.max_frame_buffer_size);
  shim_finish(ctx, 0);
}

void shim_sceVideodecCreateDecoder(GuestContext *ctx) {
  if (!ctx)
    return;
  if (!ctx->rdi || !ctx->rsi || !ctx->rdx) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }

  Ps4VideodecConfigInfo config;
  Ps4VideodecResourceInfo resources;
  if (!recomp_guest_memory_read(ctx, ctx->rdi, &config, sizeof(config)) ||
      !recomp_guest_memory_read(ctx, ctx->rsi, &resources, sizeof(resources))) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }
  if (config.this_size != sizeof(config) ||
      resources.this_size != sizeof(resources)) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_STRUCT_SIZE);
    return;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  if (config.max_frame_width > 0 && config.max_frame_height > 0) {
    width = (uint32_t)config.max_frame_width;
    height = (uint32_t)config.max_frame_height;
  } else {
    width = config.max_level >= 150 ? 3840U : 1920U;
    height = config.max_level >= 150 ? 2160U : 1080U;
  }
  if (width > PS4_VIDEODEC_MAX_WIDTH || height > PS4_VIDEODEC_MAX_HEIGHT ||
      width < 16 || height < 16 ||
      resources.max_frame_buffer_size == 0 ||
      resources.max_frame_buffer_size > PS4_VIDEODEC_MAX_FRAME_SIZE) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_CONFIG_INFO);
    return;
  }

  pthread_once(&g_ffmpeg_once, ffmpeg_initialize);
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_CODEC_TYPE);
    return;
  }
  AVCodecContext *avctx = avcodec_alloc_context3(codec);
  if (!avctx) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL);
    return;
  }
  avctx->width = (int)width;
  avctx->height = (int)height;
  avctx->thread_count = 1;
  if (avcodec_open2(avctx, codec, NULL) < 0) {
    avcodec_free_context(&avctx);
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL);
    return;
  }

  uint64_t handle = 0;
  pthread_mutex_lock(&g_decoder_mutex);
  size_t slot = PS4_VIDEODEC_MAX_DECODERS;
  for (size_t i = 0; i < PS4_VIDEODEC_MAX_DECODERS; ++i) {
    if (!g_decoders[i].codec) {
      slot = i;
      break;
    }
  }
  if (slot < PS4_VIDEODEC_MAX_DECODERS) {
    Ps4VideodecInstance *instance = &g_decoders[slot];
    instance->generation =
        (instance->generation + 1) & PS4_VIDEODEC_HANDLE_GENERATION_MASK;
    if (!instance->generation)
      instance->generation = 1;
    instance->codec = avctx;
    instance->max_frame_buffer_size = resources.max_frame_buffer_size;
    instance->max_width = width;
    instance->max_height = height;
    instance->logged_first_frame = 0;
    handle = make_handle(slot, instance->generation);
  }
  pthread_mutex_unlock(&g_decoder_mutex);
  if (slot == PS4_VIDEODEC_MAX_DECODERS) {
    avcodec_free_context(&avctx);
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL);
    return;
  }

  const Ps4VideodecCtrl control = {
      .this_size = sizeof(Ps4VideodecCtrl), .handle = handle, .version = 1};
  if (!recomp_guest_memory_write(ctx, ctx->rdx, &control, sizeof(control))) {
    pthread_mutex_lock(&g_decoder_mutex);
    Ps4VideodecInstance *instance = find_instance_locked(handle);
    if (instance)
      destroy_instance_locked(instance);
    pthread_mutex_unlock(&g_decoder_mutex);
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }

  fprintf(stderr, "[videodec] CreateDecoder H.264 handle=0x%llx max=%ux%u\n",
          (unsigned long long)handle, width, height);
  shim_finish(ctx, 0);
}

void shim_sceVideodecReset(GuestContext *ctx) {
  if (!ctx)
    return;
  Ps4VideodecCtrl control;
  if (!ctx->rdi || !recomp_guest_memory_read(ctx, ctx->rdi, &control,
                                             sizeof(control))) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }
  if (control.this_size != sizeof(control)) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_STRUCT_SIZE);
    return;
  }

  pthread_mutex_lock(&g_decoder_mutex);
  Ps4VideodecInstance *instance = find_instance_locked(control.handle);
  if (instance)
    avcodec_flush_buffers(instance->codec);
  pthread_mutex_unlock(&g_decoder_mutex);
  shim_finish(ctx, instance ? 0 : (int32_t)ORBIS_VIDEODEC_ERROR_HANDLE);
}

static int32_t decode_one(GuestContext *ctx, uint64_t control_addr,
                          uint64_t input_addr, uint64_t framebuffer_addr,
                          uint64_t picture_addr, int flush) {
  if (!control_addr || !framebuffer_addr || !picture_addr ||
      (!flush && !input_addr))
    return (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;

  Ps4VideodecCtrl control;
  Ps4VideodecFrameBuffer framebuffer;
  Ps4VideodecInputData input;
  uint64_t input_picture_size = 0;
  memset(&input, 0, sizeof(input));
  if (!recomp_guest_memory_read(ctx, control_addr, &control, sizeof(control)) ||
      !recomp_guest_memory_read(ctx, framebuffer_addr, &framebuffer,
                                sizeof(framebuffer)) ||
      (!flush && !recomp_guest_memory_read(ctx, input_addr, &input,
                                           sizeof(input))) ||
      !recomp_guest_memory_read(ctx, picture_addr, &input_picture_size,
                                sizeof(input_picture_size)))
    return (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;

  if (control.this_size != sizeof(control) ||
      framebuffer.this_size != sizeof(framebuffer) ||
      input_picture_size != sizeof(Ps4VideodecPictureInfo) ||
      (!flush && input.this_size != sizeof(input)))
    return (int32_t)ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;

  Ps4VideodecPictureInfo picture;
  memset(&picture, 0, sizeof(picture));
  picture.this_size = sizeof(picture);
  if (!recomp_guest_memory_write(ctx, picture_addr, &picture, sizeof(picture)))
    return (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;

  if (!framebuffer.frame_buffer)
    return (int32_t)ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_POINTER;
  if (framebuffer.frame_buffer_size == 0 ||
      framebuffer.frame_buffer_size > PS4_VIDEODEC_MAX_FRAME_SIZE)
    return (int32_t)ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE;

  uint8_t *access_unit = NULL;
  if (!flush) {
    if (!input.access_unit)
      return (int32_t)ORBIS_VIDEODEC_ERROR_AU_POINTER;
    if (input.access_unit_size == 0 ||
        input.access_unit_size > PS4_VIDEODEC_MAX_ACCESS_UNIT ||
        input.access_unit_size > INT_MAX)
      return (int32_t)ORBIS_VIDEODEC_ERROR_AU_SIZE;
    access_unit = (uint8_t *)malloc((size_t)input.access_unit_size);
    if (!access_unit)
      return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
    if (!recomp_guest_memory_read(ctx, input.access_unit, access_unit,
                                  (size_t)input.access_unit_size)) {
      free(access_unit);
      return (int32_t)ORBIS_VIDEODEC_ERROR_AU_POINTER;
    }
  }

  uint8_t *pixels = NULL;
  size_t pixels_size = 0;
  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = flush ? NULL : av_packet_alloc();
  if (!frame || (!flush && !packet)) {
    av_frame_free(&frame);
    av_packet_free(&packet);
    free(access_unit);
    return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
  }
  if (!flush) {
    if (av_new_packet(packet, (int)input.access_unit_size) < 0) {
      av_frame_free(&frame);
      av_packet_free(&packet);
      free(access_unit);
      return (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
    }
    memcpy(packet->data, access_unit, (size_t)input.access_unit_size);
    packet->pts = (int64_t)input.pts;
    packet->dts = (int64_t)input.dts;
  }
  free(access_unit);

  int32_t result = 0;
  uint64_t fallback_pts = flush ? 0 : input.pts;
  uint64_t attached_data = flush ? 0 : input.attached_data;
  int have_frame = 0;
  pthread_mutex_lock(&g_decoder_mutex);
  Ps4VideodecInstance *instance = find_instance_locked(control.handle);
  if (!instance) {
    result = (int32_t)ORBIS_VIDEODEC_ERROR_HANDLE;
  } else {
    int sent = avcodec_send_packet(instance->codec, packet);
    if (sent == AVERROR(EAGAIN)) {
      const int received = avcodec_receive_frame(instance->codec, frame);
      if (received >= 0) {
        have_frame = 1;
        sent = flush ? AVERROR_EOF : avcodec_send_packet(instance->codec, packet);
      }
    }
    if (sent < 0 && sent != AVERROR_EOF) {
      result = (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
    } else if (!have_frame) {
      const int received = avcodec_receive_frame(instance->codec, frame);
      if (received >= 0)
        have_frame = 1;
      else if (received != AVERROR(EAGAIN) && received != AVERROR_EOF)
        result = (int32_t)ORBIS_VIDEODEC_ERROR_API_FAIL;
    }

    if (result == 0 && have_frame) {
      result = convert_frame_to_nv12(instance, frame, fallback_pts,
                                     attached_data,
                                     framebuffer.frame_buffer_size, &pixels,
                                     &pixels_size, &picture);
      if (result == 0 && !instance->logged_first_frame) {
        instance->logged_first_frame = 1;
        fprintf(stderr,
                "[videodec] first H.264 frame %ux%u pitch=%u height=%u pts=%" PRIu64 "\n",
                picture.frame_width, picture.frame_height, picture.frame_pitch,
                picture.frame_height, picture.pts);
      }
    }
  }
  pthread_mutex_unlock(&g_decoder_mutex);
  av_frame_free(&frame);
  av_packet_free(&packet);

  if (result == 0) {
    result = write_picture(ctx, picture_addr, &framebuffer, &picture, pixels,
                           pixels_size);
  }
  free(pixels);
  return result;
}

void shim_sceVideodecDecode(GuestContext *ctx) {
  if (!ctx)
    return;
  const int32_t result = decode_one(ctx, ctx->rdi, ctx->rsi, ctx->rdx,
                                    ctx->rcx, 0);
  shim_finish(ctx, result);
}

void shim_sceVideodecFlush(GuestContext *ctx) {
  if (!ctx)
    return;
  const int32_t result = decode_one(ctx, ctx->rdi, 0, ctx->rsi, ctx->rdx, 1);
  shim_finish(ctx, result);
}

void shim_sceVideodecDeleteDecoder(GuestContext *ctx) {
  if (!ctx)
    return;
  if (!ctx->rdi) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }
  Ps4VideodecCtrl control;
  if (!recomp_guest_memory_read(ctx, ctx->rdi, &control, sizeof(control))) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
    return;
  }
  if (control.this_size != sizeof(control)) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_STRUCT_SIZE);
    return;
  }

  pthread_mutex_lock(&g_decoder_mutex);
  Ps4VideodecInstance *instance = find_instance_locked(control.handle);
  if (instance)
    destroy_instance_locked(instance);
  pthread_mutex_unlock(&g_decoder_mutex);
  if (!instance) {
    shim_finish(ctx, (int32_t)ORBIS_VIDEODEC_ERROR_HANDLE);
    return;
  }

  control.handle = 0;
  const int32_t result = recomp_guest_memory_write(
                             ctx, ctx->rdi, &control, sizeof(control))
                             ? 0
                             : (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
  shim_finish(ctx, result);
}
