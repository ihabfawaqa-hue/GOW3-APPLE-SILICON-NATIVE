// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3gpu_gnm_adapter.h"

#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GOW3GPU_GNM_ERROR UINT32_C(0x80d11000)
#define GOW3GPU_MAX_SUBMIT_BUFFERS 2048u
#define GOW3GPU_MAX_COMMAND_BUFFER_BYTES UINT32_C(0x003ffffc)
#define GOW3GPU_MAX_SUBMISSION_BYTES (64u * 1024u * 1024u)
#define GOW3GPU_MAX_RETAINED_BYTES (128u * 1024u * 1024u)

typedef struct RetainedSubmission {
  struct RetainedSubmission *next;
  uint32_t count;
  const uint32_t **dcb;
  uint32_t *dcb_sizes;
  const uint32_t **ccb;
  uint32_t *ccb_sizes;
  uint64_t bytes;
} RetainedSubmission;

static pthread_mutex_t g_submit_mutex = PTHREAD_MUTEX_INITIALIZER;
static RetainedSubmission *g_retained_submissions;
static uint64_t g_retained_bytes;
static Gow3GpuGuestMemoryApi g_backend_memory_api;
static Ps4Gow3GpuSubmitCommandBuffersFn g_backend_submit;
static Ps4Gow3GpuSubmitDoneFn g_backend_submit_done;
static Ps4Gow3GpuWaitIdleFn g_backend_wait_idle;
static atomic_int g_backend_state;

static void free_submission(RetainedSubmission *submission) {
  if (!submission) return;
  for (uint32_t i = 0; i < submission->count; ++i) {
    if (submission->dcb) free((void *)submission->dcb[i]);
    if (submission->ccb) free((void *)submission->ccb[i]);
  }
  free(submission->dcb);
  free(submission->dcb_sizes);
  free(submission->ccb);
  free(submission->ccb_sizes);
  free(submission);
}

static void free_all_retained(void) {
  RetainedSubmission *submission = g_retained_submissions;
  g_retained_submissions = NULL;
  g_retained_bytes = 0;
  while (submission) {
    RetainedSubmission *next = submission->next;
    free_submission(submission);
    submission = next;
  }
}

static int valid_memory_api(const Gow3GpuGuestMemoryApi *api) {
  return api && api->abi_version == GOW3GPU_GUEST_MEMORY_ABI_VERSION &&
         api->struct_size >= sizeof(Gow3GpuGuestMemoryApi) && api->read &&
         api->is_mapped;
}

static int read_guest(const Gow3GpuGuestMemoryApi *api, uint64_t address,
                      void *destination, uint64_t size) {
  return size != 0 && address != 0 && destination &&
         api->is_mapped(api->user_context, address, size) &&
         api->read(api->user_context, address, destination, size);
}

static void cleanup_unsubmitted(RetainedSubmission *submission) {
  free_submission(submission);
}

static int copy_input_array(const Gow3GpuGuestMemoryApi *api, const void *source,
                            void *destination, uint64_t size) {
  if (!source || !destination || !size) return 0;
  uint64_t guest_address = 0;
  if (api->host_to_guest(api->user_context, source, &guest_address))
    return read_guest(api, guest_address, destination, size);
  /* A few internal GNM helpers submit stack-local one-element arrays. Those
   * never enter the guest import ABI; the caller owns them for this call. */
  memcpy(destination, source, (size_t)size);
  return 1;
}

int ps4_gow3gpu_install_submission_backend(
    const Gow3GpuGuestMemoryApi *memory_api,
    Ps4Gow3GpuSubmitCommandBuffersFn submit,
    Ps4Gow3GpuSubmitDoneFn submit_done,
    Ps4Gow3GpuWaitIdleFn wait_idle) {
  if (!valid_memory_api(memory_api) || !memory_api->host_to_guest ||
      !submit || !submit_done || !wait_idle)
    return -1;
  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &g_backend_state, &expected, -1, memory_order_acq_rel,
          memory_order_acquire))
    return -2;
  g_backend_memory_api = *memory_api;
  g_backend_submit = submit;
  g_backend_submit_done = submit_done;
  g_backend_wait_idle = wait_idle;
  atomic_store_explicit(&g_backend_state, 1, memory_order_release);
  return 0;
}

int ps4_gow3gpu_submission_backend_active(void) {
  return atomic_load_explicit(&g_backend_state, memory_order_acquire) == 1;
}

int32_t ps4_gow3gpu_submit_guest_arrays(
    GuestContext *ctx, uint32_t count, const uint64_t *dcb_guest_addresses,
    const uint32_t *dcb_sizes, const uint64_t *ccb_guest_addresses,
    const uint32_t *ccb_sizes) {
  if (!ctx || !ps4_gow3gpu_submission_backend_active())
    return (int32_t)GOW3GPU_GNM_ERROR;
  if (count > GOW3GPU_MAX_SUBMIT_BUFFERS ||
      ((ccb_guest_addresses == NULL) != (ccb_sizes == NULL)))
    return (int32_t)GOW3GPU_GNM_ERROR;
  if (count == 0)
    return g_backend_submit(0, NULL, NULL, NULL, NULL);
  if (!dcb_guest_addresses || !dcb_sizes)
    return (int32_t)GOW3GPU_GNM_ERROR;

  RetainedSubmission *submission =
      (RetainedSubmission *)calloc(1, sizeof(*submission));
  if (!submission) return (int32_t)GOW3GPU_GNM_ERROR;
  submission->count = count;
  submission->dcb = (const uint32_t **)calloc(count, sizeof(*submission->dcb));
  submission->dcb_sizes =
      (uint32_t *)calloc(count, sizeof(*submission->dcb_sizes));
  if (ccb_guest_addresses) {
    submission->ccb =
        (const uint32_t **)calloc(count, sizeof(*submission->ccb));
    submission->ccb_sizes =
        (uint32_t *)calloc(count, sizeof(*submission->ccb_sizes));
  }
  uint64_t *dcb_addresses =
      (uint64_t *)calloc(count, sizeof(*dcb_addresses));
  uint64_t *ccb_addresses = ccb_guest_addresses
      ? (uint64_t *)calloc(count, sizeof(*ccb_addresses))
      : NULL;
  const uint64_t address_bytes = (uint64_t)count * sizeof(uint64_t);
  const uint64_t size_bytes = (uint64_t)count * sizeof(uint32_t);
  if (!submission->dcb || !submission->dcb_sizes || !dcb_addresses ||
      (ccb_guest_addresses &&
       (!submission->ccb || !submission->ccb_sizes || !ccb_addresses)) ||
      !copy_input_array(&g_backend_memory_api, dcb_guest_addresses,
                        dcb_addresses, address_bytes) ||
      !copy_input_array(&g_backend_memory_api, dcb_sizes,
                        submission->dcb_sizes, size_bytes) ||
      (ccb_guest_addresses &&
       (!copy_input_array(&g_backend_memory_api, ccb_guest_addresses,
                          ccb_addresses, address_bytes) ||
        !copy_input_array(&g_backend_memory_api, ccb_sizes,
                          submission->ccb_sizes, size_bytes)))) {
    free(dcb_addresses);
    free(ccb_addresses);
    cleanup_unsubmitted(submission);
    return (int32_t)GOW3GPU_GNM_ERROR;
  }

  int valid = 1;
  for (uint32_t i = 0; i < count && valid; ++i) {
    const uint32_t dcb_size = submission->dcb_sizes[i];
    if (!dcb_size || dcb_size > GOW3GPU_MAX_COMMAND_BUFFER_BYTES ||
        (dcb_size & 3u) ||
        submission->bytes > GOW3GPU_MAX_SUBMISSION_BYTES - dcb_size) {
      valid = 0;
      break;
    }
    void *dcb_copy = malloc(dcb_size);
    if (!dcb_copy || !read_guest(&g_backend_memory_api, dcb_addresses[i],
                                 dcb_copy, dcb_size)) {
      free(dcb_copy);
      valid = 0;
      break;
    }
    submission->dcb[i] = (const uint32_t *)dcb_copy;
    submission->bytes += dcb_size;

    if (submission->ccb) {
      const uint32_t ccb_size = submission->ccb_sizes[i];
      if (ccb_size > GOW3GPU_MAX_COMMAND_BUFFER_BYTES || (ccb_size & 3u) ||
          submission->bytes > GOW3GPU_MAX_SUBMISSION_BYTES - ccb_size) {
        valid = 0;
        break;
      }
      if (ccb_size) {
        void *ccb_copy = malloc(ccb_size);
        if (!ccb_copy || !read_guest(&g_backend_memory_api, ccb_addresses[i],
                                     ccb_copy, ccb_size)) {
          free(ccb_copy);
          valid = 0;
          break;
        }
        submission->ccb[i] = (const uint32_t *)ccb_copy;
      }
      submission->bytes += ccb_size;
    }
  }
  free(dcb_addresses);
  free(ccb_addresses);
  if (!valid) {
    cleanup_unsubmitted(submission);
    return (int32_t)GOW3GPU_GNM_ERROR;
  }

  pthread_mutex_lock(&g_submit_mutex);
  if (submission->bytes > GOW3GPU_MAX_RETAINED_BYTES - g_retained_bytes) {
    pthread_mutex_unlock(&g_submit_mutex);
    cleanup_unsubmitted(submission);
    return (int32_t)GOW3GPU_GNM_ERROR;
  }
  const int32_t result = g_backend_submit(
      count, submission->dcb, submission->dcb_sizes, submission->ccb,
      submission->ccb_sizes);
  if (result == 0) {
    submission->next = g_retained_submissions;
    g_retained_submissions = submission;
    g_retained_bytes += submission->bytes;
  } else {
    cleanup_unsubmitted(submission);
  }
  pthread_mutex_unlock(&g_submit_mutex);
  return result;
}

int32_t ps4_gow3gpu_backend_submit_done(void) {
  if (!ps4_gow3gpu_submission_backend_active())
    return (int32_t)GOW3GPU_GNM_ERROR;
  return ps4_gow3gpu_submit_done(g_backend_submit_done,
                                 g_backend_wait_idle);
}

int32_t ps4_gow3gpu_submit_from_context(
    GuestContext *ctx, const Gow3GpuGuestMemoryApi *memory_api,
    Ps4Gow3GpuSubmitCommandBuffersFn submit) {
  if (!ctx || !valid_memory_api(memory_api) || !submit)
    return (int32_t)GOW3GPU_GNM_ERROR;

  const uint32_t count = (uint32_t)ctx->rdi;
  const uint64_t dcb_addresses_va = ctx->rsi;
  const uint64_t dcb_sizes_va = ctx->rdx;
  const uint64_t ccb_addresses_va = ctx->rcx;
  const uint64_t ccb_sizes_va = ctx->r8;

  if (count > GOW3GPU_MAX_SUBMIT_BUFFERS ||
      ((ccb_addresses_va == 0) != (ccb_sizes_va == 0)))
    return (int32_t)GOW3GPU_GNM_ERROR;

  if (count == 0) {
    return submit(0, NULL, NULL, NULL, NULL);
  }

  if (dcb_addresses_va == 0 || dcb_sizes_va == 0)
    return (int32_t)GOW3GPU_GNM_ERROR;

  RetainedSubmission *submission =
      (RetainedSubmission *)calloc(1, sizeof(*submission));
  if (!submission) return (int32_t)GOW3GPU_GNM_ERROR;
  submission->count = count;
  submission->dcb = (const uint32_t **)calloc(count, sizeof(*submission->dcb));
  submission->dcb_sizes = (uint32_t *)calloc(count, sizeof(*submission->dcb_sizes));
  if (ccb_addresses_va) {
    submission->ccb = (const uint32_t **)calloc(count, sizeof(*submission->ccb));
    submission->ccb_sizes = (uint32_t *)calloc(count, sizeof(*submission->ccb_sizes));
  }
  uint64_t *dcb_guest_addresses =
      (uint64_t *)calloc(count, sizeof(*dcb_guest_addresses));
  uint64_t *ccb_guest_addresses = ccb_addresses_va
      ? (uint64_t *)calloc(count, sizeof(*ccb_guest_addresses))
      : NULL;

  if (!submission->dcb || !submission->dcb_sizes || !dcb_guest_addresses ||
      (ccb_addresses_va && (!submission->ccb || !submission->ccb_sizes ||
                            !ccb_guest_addresses))) {
    free(dcb_guest_addresses);
    free(ccb_guest_addresses);
    cleanup_unsubmitted(submission);
    return (int32_t)GOW3GPU_GNM_ERROR;
  }

  const uint64_t address_bytes = (uint64_t)count * sizeof(uint64_t);
  const uint64_t size_bytes = (uint64_t)count * sizeof(uint32_t);
  if (!read_guest(memory_api, dcb_addresses_va, dcb_guest_addresses,
                  address_bytes) ||
      !read_guest(memory_api, dcb_sizes_va, submission->dcb_sizes, size_bytes) ||
      (ccb_addresses_va &&
       (!read_guest(memory_api, ccb_addresses_va, ccb_guest_addresses,
                    address_bytes) ||
        !read_guest(memory_api, ccb_sizes_va, submission->ccb_sizes,
                    size_bytes)))) {
    free(dcb_guest_addresses);
    free(ccb_guest_addresses);
    cleanup_unsubmitted(submission);
    return (int32_t)GOW3GPU_GNM_ERROR;
  }

  int valid = 1;
  for (uint32_t i = 0; i < count && valid; ++i) {
    const uint32_t dcb_size = submission->dcb_sizes[i];
    if (!dcb_size || dcb_size > GOW3GPU_MAX_COMMAND_BUFFER_BYTES ||
        (dcb_size & 3u) || submission->bytes > GOW3GPU_MAX_SUBMISSION_BYTES - dcb_size) {
      valid = 0;
      break;
    }
    void *dcb_copy = malloc(dcb_size);
    if (!dcb_copy || !read_guest(memory_api, dcb_guest_addresses[i], dcb_copy,
                                 dcb_size)) {
      free(dcb_copy);
      valid = 0;
      break;
    }
    submission->dcb[i] = (const uint32_t *)dcb_copy;
    submission->bytes += dcb_size;

    if (submission->ccb) {
      const uint32_t ccb_size = submission->ccb_sizes[i];
      if (ccb_size > GOW3GPU_MAX_COMMAND_BUFFER_BYTES || (ccb_size & 3u) ||
          submission->bytes > GOW3GPU_MAX_SUBMISSION_BYTES - ccb_size) {
        valid = 0;
        break;
      }
      if (ccb_size) {
        void *ccb_copy = malloc(ccb_size);
        if (!ccb_copy || !read_guest(memory_api, ccb_guest_addresses[i], ccb_copy,
                                     ccb_size)) {
          free(ccb_copy);
          valid = 0;
          break;
        }
        submission->ccb[i] = (const uint32_t *)ccb_copy;
      }
      submission->bytes += ccb_size;
    }
  }

  free(dcb_guest_addresses);
  free(ccb_guest_addresses);
  if (!valid) {
    cleanup_unsubmitted(submission);
    return (int32_t)GOW3GPU_GNM_ERROR;
  }

  pthread_mutex_lock(&g_submit_mutex);
  if (submission->bytes > GOW3GPU_MAX_RETAINED_BYTES - g_retained_bytes) {
    pthread_mutex_unlock(&g_submit_mutex);
    cleanup_unsubmitted(submission);
    return (int32_t)GOW3GPU_GNM_ERROR;
  }
  const int32_t result = submit(count, submission->dcb, submission->dcb_sizes,
                                submission->ccb, submission->ccb_sizes);
  if (result == 0) {
    submission->next = g_retained_submissions;
    g_retained_submissions = submission;
    g_retained_bytes += submission->bytes;
  } else {
    cleanup_unsubmitted(submission);
  }
  pthread_mutex_unlock(&g_submit_mutex);
  return result;
}

int32_t ps4_gow3gpu_submit_done(Ps4Gow3GpuSubmitDoneFn submit_done,
                                Ps4Gow3GpuWaitIdleFn wait_idle) {
  if (!submit_done || !wait_idle) return (int32_t)GOW3GPU_GNM_ERROR;
  pthread_mutex_lock(&g_submit_mutex);
  int32_t result = submit_done();
  if (result == 0) result = wait_idle();
  if (result == 0) free_all_retained();
  pthread_mutex_unlock(&g_submit_mutex);
  return result;
}

uint64_t ps4_gow3gpu_retained_command_bytes(void) {
  pthread_mutex_lock(&g_submit_mutex);
  const uint64_t bytes = g_retained_bytes;
  pthread_mutex_unlock(&g_submit_mutex);
  return bytes;
}
