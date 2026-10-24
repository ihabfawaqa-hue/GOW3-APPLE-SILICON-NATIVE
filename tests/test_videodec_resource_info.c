// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_videodec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORBIS_VIDEODEC_ERROR_STRUCT_SIZE 0x80C10002U
#define ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER 0x80C1000FU

static uint8_t guest_memory[5 * 1024 * 1024];

bool recomp_guest_memory_read(GuestContext *ctx, uint64_t guest_addr,
                              void *destination, size_t size) {
  (void)ctx;
  if (!destination || guest_addr > sizeof(guest_memory) ||
      size > sizeof(guest_memory) - (size_t)guest_addr)
    return false;
  memcpy(destination, guest_memory + guest_addr, size);
  return true;
}

bool recomp_guest_memory_write(GuestContext *ctx, uint64_t guest_addr,
                               const void *source, size_t size) {
  (void)ctx;
  if (!source || guest_addr > sizeof(guest_memory) ||
      size > sizeof(guest_memory) - (size_t)guest_addr)
    return false;
  memcpy(guest_memory + guest_addr, source, size);
  return true;
}

static void test_1080p_query(void) {
  memset(guest_memory, 0, sizeof(guest_memory));
  Ps4VideodecConfigInfo config = {
      .this_size = sizeof(config),
      .codec_type = 1,
      .profile = 1,
      .max_level = 40,
      .max_frame_width = 1920,
      .max_frame_height = 1080,
      .max_dpb_frame_count = 8,
  };
  Ps4VideodecResourceInfo resource_request = {
      .this_size = sizeof(resource_request),
  };
  memcpy(guest_memory + 0x100, &config, sizeof(config));
  memcpy(guest_memory + 0x200, &resource_request, sizeof(resource_request));

  GuestContext ctx = {0};
  ctx.rdi = 0x100;
  ctx.rsi = 0x200;
  shim_sceVideodecQueryResourceInfo(&ctx);

  assert((int32_t)ctx.rax == 0);
  assert(ctx.rsp == 8);
  Ps4VideodecResourceInfo result;
  memcpy(&result, guest_memory + 0x200, sizeof(result));
  const uint64_t frame_size = (2048ULL * 1088ULL * 3ULL) / 2ULL;
  const uint64_t padded_frame_size = frame_size + 0x4000ULL;
  assert(result.this_size == sizeof(result));
  assert(result.cpu_memory_size == 16ULL * 1024ULL * 1024ULL);
  assert(result.cpu_memory == 0);
  assert(result.cpu_gpu_memory_size ==
         padded_frame_size * 10ULL + 8ULL * 1024ULL * 1024ULL);
  assert(result.cpu_gpu_memory == 0);
  assert(result.max_frame_buffer_size == padded_frame_size);
  assert(result.frame_buffer_alignment == 0x100);
}

static void test_4k_level_fallback(void) {
  Ps4VideodecConfigInfo config = {
      .this_size = sizeof(config),
      .max_level = 150,
      .max_dpb_frame_count = 8,
  };
  Ps4VideodecResourceInfo result;
  assert(ps4_videodec_compute_resource_info(&config, &result) == 0);
  const uint64_t frame_size = (3840ULL * 2160ULL * 3ULL) / 2ULL;
  const uint64_t padded_frame_size =
      ((frame_size + 255ULL) & ~255ULL) + 0x4000ULL;
  assert(result.max_frame_buffer_size == padded_frame_size);
}

static void test_invalid_guest_ranges_and_sizes(void) {
  memset(guest_memory, 0, sizeof(guest_memory));
  Ps4VideodecConfigInfo config = {.this_size = 1};
  uint64_t output_size = sizeof(Ps4VideodecResourceInfo);
  memcpy(guest_memory + 0x100, &config, sizeof(config));
  memcpy(guest_memory + 0x200, &output_size, sizeof(output_size));

  GuestContext ctx = {0};
  ctx.rdi = 0x100;
  ctx.rsi = 0x200;
  shim_sceVideodecQueryResourceInfo(&ctx);
  assert((int32_t)ctx.rax == (int32_t)ORBIS_VIDEODEC_ERROR_STRUCT_SIZE);
  assert(ctx.rsp == 8);

  memset(&ctx, 0, sizeof(ctx));
  ctx.rdi = 0x100;
  ctx.rsi = sizeof(guest_memory) - 4;
  shim_sceVideodecQueryResourceInfo(&ctx);
  assert((int32_t)ctx.rax == (int32_t)ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER);
  assert(ctx.rsp == 8);
}

static void test_h264_decode(const char *bitstream_path) {
  FILE *stream = fopen(bitstream_path, "rb");
  assert(stream);
  assert(fseek(stream, 0, SEEK_END) == 0);
  const long encoded_size = ftell(stream);
  assert(encoded_size > 0 && encoded_size < 1024 * 1024);
  rewind(stream);
  uint8_t *encoded = (uint8_t *)malloc((size_t)encoded_size);
  assert(encoded);
  assert(fread(encoded, 1, (size_t)encoded_size, stream) ==
         (size_t)encoded_size);
  fclose(stream);

  memset(guest_memory, 0, sizeof(guest_memory));
  const uint64_t config_addr = 0x100;
  const uint64_t resources_addr = 0x200;
  const uint64_t control_addr = 0x300;
  const uint64_t input_addr = 0x400;
  const uint64_t framebuffer_info_addr = 0x500;
  const uint64_t picture_addr = 0x600;
  const uint64_t bitstream_addr = 0x1000;
  const uint64_t frame_pixels_addr = 0x20000;

  Ps4VideodecConfigInfo config = {
      .this_size = sizeof(config),
      .codec_type = 0,
      .max_frame_width = 64,
      .max_frame_height = 48,
      .max_dpb_frame_count = 2,
  };
  Ps4VideodecResourceInfo resources = {
      .this_size = sizeof(resources),
  };
  assert(ps4_videodec_compute_resource_info(&config, &resources) == 0);
  Ps4VideodecCtrl control = {.this_size = sizeof(control)};
  Ps4VideodecInputData input = {
      .this_size = sizeof(input),
      .access_unit = bitstream_addr,
      .access_unit_size = (uint64_t)encoded_size,
      .pts = 1,
      .dts = 1,
      .attached_data = 0x1234,
  };
  Ps4VideodecFrameBuffer framebuffer = {
      .this_size = sizeof(framebuffer),
      .frame_buffer = frame_pixels_addr,
      .frame_buffer_size = resources.max_frame_buffer_size,
  };
  Ps4VideodecPictureInfo picture = {
      .this_size = sizeof(picture),
  };
  memcpy(guest_memory + config_addr, &config, sizeof(config));
  memcpy(guest_memory + resources_addr, &resources, sizeof(resources));
  memcpy(guest_memory + control_addr, &control, sizeof(control));
  memcpy(guest_memory + input_addr, &input, sizeof(input));
  memcpy(guest_memory + framebuffer_info_addr, &framebuffer,
         sizeof(framebuffer));
  memcpy(guest_memory + picture_addr, &picture, sizeof(picture));
  memcpy(guest_memory + bitstream_addr, encoded, (size_t)encoded_size);
  free(encoded);

  GuestContext ctx = {0};
  ctx.rdi = config_addr;
  ctx.rsi = resources_addr;
  ctx.rdx = control_addr;
  shim_sceVideodecCreateDecoder(&ctx);
  assert((int32_t)ctx.rax == 0);
  assert(ctx.rsp == 8);
  memcpy(&control, guest_memory + control_addr, sizeof(control));
  assert(control.this_size == sizeof(control) && control.handle != 0);

  memset(&ctx, 0, sizeof(ctx));
  ctx.rdi = control_addr;
  shim_sceVideodecReset(&ctx);
  assert((int32_t)ctx.rax == 0);

  memset(&ctx, 0, sizeof(ctx));
  ctx.rdi = control_addr;
  ctx.rsi = input_addr;
  ctx.rdx = framebuffer_info_addr;
  ctx.rcx = picture_addr;
  shim_sceVideodecDecode(&ctx);
  assert((int32_t)ctx.rax == 0);
  memcpy(&picture, guest_memory + picture_addr, sizeof(picture));
  assert(picture.this_size == sizeof(picture));
  assert(picture.is_valid == 1);
  assert(picture.frame_width == 64);
  assert(picture.frame_pitch == 64);
  assert(picture.frame_height == 48);
  assert(picture.attached_data == 0x1234);
  const size_t luma_size = picture.frame_pitch * picture.frame_height;
  const size_t sample = (size_t)24 * picture.frame_pitch + 32;
  const uint8_t luma = guest_memory[frame_pixels_addr + sample];
  const uint8_t cb = guest_memory[frame_pixels_addr + luma_size +
                                  (size_t)12 * picture.frame_pitch + 32];
  const uint8_t cr = guest_memory[frame_pixels_addr + luma_size +
                                  (size_t)12 * picture.frame_pitch + 33];
  assert(luma > 50 && luma < 120);
  assert(cb > 50 && cb < 150);
  assert(cr > 150);

  memset(&ctx, 0, sizeof(ctx));
  ctx.rdi = control_addr;
  shim_sceVideodecDeleteDecoder(&ctx);
  assert((int32_t)ctx.rax == 0);
  memcpy(&control, guest_memory + control_addr, sizeof(control));
  assert(control.handle == 0);

  memset(&ctx, 0, sizeof(ctx));
  ctx.rdi = control_addr;
  shim_sceVideodecDeleteDecoder(&ctx);
  assert((int32_t)ctx.rax == (int32_t)0x80C10003U);
}

int main(int argc, char **argv) {
  test_1080p_query();
  test_4k_level_fallback();
  test_invalid_guest_ranges_and_sizes();
  if (argc > 1)
    test_h264_decode(argv[1]);
  puts("PASS: videodec sizing, checked ABI, H.264 decode, NV12 output, and handles");
  return 0;
}
