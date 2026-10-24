// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3gpu_runtime_adapter.h"
#include "gow3gpu_gnm_adapter.h"
#include "recomp_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

void ps4_gnm_guest_remapped(uint64_t address, uint64_t size);
void ps4_gnm_notify_cpu_write(uint64_t address, uint64_t size);

static pthread_mutex_t g_direct_access_mutex = PTHREAD_MUTEX_INITIALIZER;

void ps4_direct_mem_guest_access_lock(void) {
  pthread_mutex_lock(&g_direct_access_mutex);
}

void ps4_direct_mem_guest_access_unlock(void) {
  pthread_mutex_unlock(&g_direct_access_mutex);
}

static Gow3GpuGuestMemoryApi g_memory_api;
static Gow3GpuHostRuntimeApi g_host_api;
static int g_map_count;
static int g_unmap_count;
static int g_invalidate_count;
static uint64_t g_last_address;
static uint64_t g_last_size;
static const uint32_t *g_submitted_dcb;
static const uint32_t *g_submitted_ccb;
static int g_submit_calls;
static int g_submit_done_calls;
static int g_primary_eop_notifications;
static int g_primary_flip_notifications;
static int32_t g_flip_handle;
static int32_t g_flip_buffer;
static int64_t g_flip_arg;

void ps4_gnm_notify_eop(void) { g_primary_eop_notifications++; }

void ps4_videoout_complete_external_flip(int32_t handle, int32_t buffer_index,
                                         int64_t flip_arg) {
  g_primary_flip_notifications++;
  g_flip_handle = handle;
  g_flip_buffer = buffer_index;
  g_flip_arg = flip_arg;
}

uint64_t sceKernelGetProcessTime(void) { return 123456; }
uint64_t sceKernelGetProcessTimeCounter(void) { return 654321; }
uint64_t sceKernelGetTscFrequency(void) { return 2130000000ULL; }

int ps4_vfs_resolve(const char *guest_path, char *host_path, size_t host_path_sz) {
  if (!guest_path || !host_path || host_path_sz < 2) return -1;
  const char resolved[] = "/tmp/mock-gow3-path";
  if (sizeof(resolved) > host_path_sz) return -1;
  memcpy(host_path, resolved, sizeof(resolved));
  return 0;
}

static int set_memory_api(const Gow3GpuGuestMemoryApi *api) {
  if (!api) return -1;
  g_memory_api = *api;
  return 0;
}

static int set_host_api(const Gow3GpuHostRuntimeApi *api) {
  if (!api) return -1;
  g_host_api = *api;
  return 0;
}

static int fault_handler(void *ucontext, void *address) {
  return ucontext == (void *)0x1234 && address == (void *)0x5678 ? 7 : 0;
}

static void on_map(void *context, uint64_t address, uint64_t size) {
  (void)context;
  g_map_count++;
  g_last_address = address;
  g_last_size = size;
}

static void on_unmap(void *context, uint64_t address, uint64_t size) {
  (void)context;
  g_unmap_count++;
  g_last_address = address;
  g_last_size = size;
}

static void on_invalidate(void *context, uint64_t address, uint64_t size) {
  (void)context;
  g_invalidate_count++;
  g_last_address = address;
  g_last_size = size;
}

static int fail(const char *message) {
  fprintf(stderr, "runtime adapter smoke failed: %s\n", message);
  return 1;
}

static int32_t fake_submit(uint32_t count, const uint32_t **dcb_addresses,
                           uint32_t *dcb_sizes, const uint32_t **ccb_addresses,
                           uint32_t *ccb_sizes) {
  if (count != 1 || !dcb_addresses || !dcb_sizes || dcb_sizes[0] != 8 ||
      !dcb_addresses[0] || dcb_addresses[0][0] != 0xc0001000u ||
      dcb_addresses[0][1] != 0 || !ccb_addresses || !ccb_sizes ||
      ccb_sizes[0] != 8 || !ccb_addresses[0] ||
      ccb_addresses[0][0] != 0xc0001000u || ccb_addresses[0][1] != 0)
    return (int32_t)0x80d11000u;
  g_submitted_dcb = dcb_addresses[0];
  g_submitted_ccb = ccb_addresses[0];
  g_submit_calls++;
  return 0;
}

static int32_t fake_submit_done(void) {
  g_submit_done_calls++;
  return 0;
}

static int32_t fake_wait_idle(void) { return 0; }

int main(void) {
  const size_t memory_size = 0x50000;
  uint8_t *memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
  if (memory == MAP_FAILED) return fail("mmap");

  GuestContext ctx = {0};
  ctx.mem_base = memory;
  ctx.mem_size = memory_size;
  ctx.heap_ptr = 0x14000;
  ctx.process_ctx = &ctx;
  pthread_mutex_init(&ctx.vm_mutex, NULL);
  GuestVMExtent image = {.addr = 0, .size = 0x14000, .is_free = false,
                         .is_pinned = true,
                         .prot = PROT_READ | PROT_WRITE};
  GuestVMExtent free_heap = {.addr = 0x14000, .size = 0x1c000, .is_free = true};
  GuestVMExtent allocated = {.addr = 0x30000, .size = 0x2000, .is_free = false,
                             .prot = PROT_NONE};
  GuestVMExtent free_tail = {.addr = 0x32000, .size = 0x1e000, .is_free = true};
  image.next = &free_heap;
  free_heap.prev = &image;
  free_heap.next = &allocated;
  allocated.prev = &free_heap;
  allocated.next = &free_tail;
  free_tail.prev = &allocated;
  ctx.vm_extents = &image;

  if (ps4_gow3gpu_install_runtime_apis(&ctx, set_memory_api, set_host_api,
                                       fault_handler) != 0)
    return fail("install API tables");
  if (g_memory_api.abi_version != GOW3GPU_GUEST_MEMORY_ABI_VERSION ||
      g_host_api.abi_version != GOW3GPU_HOST_RUNTIME_ABI_VERSION)
    return fail("ABI version");

  const int mapping_checks[] = {
      g_memory_api.is_mapped(&ctx, 0x10000, 8),
      g_memory_api.is_mapped(&ctx, 0x11ff8, 16),
      g_memory_api.is_mapped(&ctx, 0x30000, 0x2000),
      g_memory_api.is_mapped(&ctx, 0x30000, 0),
      g_memory_api.is_mapped(&ctx, 0x31ff8, 16),
      g_memory_api.is_mapped(&ctx, 0x20000, 8),
      g_memory_api.is_mapped(&ctx, 0, 8),
      g_memory_api.is_mapped(&ctx, memory_size - 4, 8)};
  if (!mapping_checks[0] || !mapping_checks[1] || !mapping_checks[2] ||
      !mapping_checks[3] || mapping_checks[4] || mapping_checks[5] ||
      mapping_checks[6] || mapping_checks[7]) {
    fprintf(stderr, "mapping checks: %d %d %d %d %d %d %d %d\n",
            mapping_checks[0], mapping_checks[1], mapping_checks[2],
            mapping_checks[3], mapping_checks[4], mapping_checks[5],
            mapping_checks[6], mapping_checks[7]);
    return fail("allocated guest mapping validation");
  }
  uint64_t guest_address = 0;
  if (!g_memory_api.host_to_guest(&ctx, memory + 0x12345, &guest_address) ||
      guest_address != 0x12345)
    return fail("host to guest mapping");
  uint64_t start = 0, end = 0;
  int mapped = 0;
  if (!g_memory_api.query_region(&ctx, 0x8000, &start, &end, &mapped) ||
      start != 0 || end != 0x10000 || mapped != 0 ||
      !g_memory_api.query_region(&ctx, 0x10000, &start, &end, &mapped) ||
      start != 0x10000 || end != 0x14000 || mapped != 1 ||
      !g_memory_api.query_region(&ctx, 0x20000, &start, &end, &mapped) ||
      start != 0x14000 || end != 0x30000 || mapped != 0 ||
      !g_memory_api.query_region(&ctx, 0x30000, &start, &end, &mapped) ||
      start != 0x30000 || end != 0x32000 || mapped != 1)
    return fail("sparse-region boundaries");

  const uint64_t value = 0x1122334455667788ULL;
  memcpy(memory + 0x10000, &value, sizeof(value));
  uint64_t loaded = 0;
  if (!g_memory_api.atomic_load(&ctx, 0x10000, 8, &loaded) || loaded != value)
    return fail("acquire atomic load");
  uint64_t previous = 0;
  if (!g_memory_api.atomic_u64(&ctx, 0x10000, GOW3GPU_ATOMIC_FETCH_ADD, 3,
                               &previous) || previous != value)
    return fail("acquire-release atomic add");
  uint64_t copied = 0;
  if (!g_memory_api.read(&ctx, 0x10000, &copied, sizeof(copied)) ||
      copied != value + 3 ||
      g_memory_api.read(&ctx, 0x20000, &copied, sizeof(copied)))
    return fail("guest read");
  const uint32_t gpu_only_value = 0x1234abcd;
  memcpy(memory + 0x30000, &gpu_only_value, sizeof(gpu_only_value));
  uint32_t gpu_only_copy = 0;
  if (!g_memory_api.read(&ctx, 0x30000, &gpu_only_copy,
                         sizeof(gpu_only_copy)) ||
      gpu_only_copy != gpu_only_value)
    return fail("GPU mapping ignores guest CPU permission bits");
  const uint32_t writeback = 0xdeadbeef;
  if (!g_memory_api.writeback(&ctx, 0x10008, &writeback, sizeof(writeback)) ||
      memcmp(memory + 0x10008, &writeback, sizeof(writeback)) != 0 ||
      g_memory_api.writeback(&ctx, 0x20000, &writeback, sizeof(writeback)))
    return fail("GPU writeback");
  if (g_memory_api.clamp_range_size(&ctx, memory_size - 16, 128) != 16 ||
      g_memory_api.clamp_range_size(&ctx, 0, 128) != 0)
    return fail("range clamp");

  // A nested callback on the same thread models page-tracking fault handling
  // re-entering while a checked guest-memory range is leased.
  RecompGuestMemoryLease outer, inner;
  if (!recomp_guest_gpu_memory_acquire(&ctx, 0x30000, 8, PROT_READ, &outer) ||
      !recomp_guest_gpu_memory_acquire(&ctx, 0x30008, 8, PROT_READ, &inner))
    return fail("reentrant GPU guest-memory lease");
  recomp_guest_memory_release(&inner);
  if (pthread_mutex_trylock(&ctx.vm_mutex) != EBUSY ||
      pthread_mutex_trylock(&g_direct_access_mutex) != EBUSY)
    return fail("outer lease lock lifetime");
  recomp_guest_memory_release(&outer);

  /* On this host the callback must accept the OS granularity, including the
   * 16 KiB page size used by Apple Silicon. */
  if (!g_memory_api.gpu_protect(&ctx, 0x10001, 1, 1, 0) ||
      !g_memory_api.read(&ctx, 0x10000, &copied, sizeof(copied)) ||
      !g_memory_api.gpu_protect(&ctx, 0x10001, 1, 1, 1))
    return fail("host page protection");

  int hook_context = 42;
  if (!g_memory_api.install_gpu_hooks(&ctx, on_map, on_unmap, on_invalidate,
                                      &hook_context) ||
      g_map_count != 2 || g_last_address != 0x30000 || g_last_size != 0x2000)
    return fail("initial mapped ranges");
  ps4_gow3gpu_guest_map(0x28000, 0x1000);
  ps4_gow3gpu_guest_unmap(0x28000, 0x1000);
  ps4_gow3gpu_guest_invalidate(0x28000, 0x1000);
  if (g_map_count != 3 || g_unmap_count != 1 || g_invalidate_count != 1 ||
      g_last_address != 0x28000 || g_last_size != 0x1000)
    return fail("VM lifecycle hooks");
  ps4_gnm_guest_remapped(0x30000, 0x2000);
  if (g_invalidate_count != 2 || g_last_address != 0x30000 ||
      g_last_size != 0x2000)
    return fail("direct-memory remap invalidation");
  ps4_gnm_notify_cpu_write(0x30000, 0x2000);
  if (g_invalidate_count != 3 || g_last_address != 0x30000 ||
      g_last_size != 0x2000)
    return fail("CPU-write invalidation");

  int32_t *host_errno = g_host_api.errno_address(&ctx);
  if (!host_errno || g_host_api.process_time_us(&ctx) != 123456 ||
      g_host_api.process_time_counter(&ctx) != 654321 ||
      g_host_api.tsc_frequency(&ctx) != 2130000000ULL)
    return fail("host errno and clocks");
  if (!g_host_api.notify_gnm_eop || !g_host_api.notify_videoout_flip)
    return fail("host completion callbacks installed");
  g_host_api.notify_gnm_eop(g_host_api.user_context);
  g_host_api.notify_videoout_flip(g_host_api.user_context, 1, 2, 0x3456);
  if (g_primary_eop_notifications != 1 || g_primary_flip_notifications != 1 ||
      g_flip_handle != 1 || g_flip_buffer != 2 || g_flip_arg != 0x3456)
    return fail("completion callbacks wake the primary runtime with exact flip state");
  char path[64] = {0};
  if (g_host_api.translate_path(&ctx, "/app0/test", path, sizeof(path)) != 0 ||
      strcmp(path, "/tmp/mock-gow3-path") != 0)
    return fail("VFS translation");
  if (g_host_api.attach_host_thread(&ctx, "gow3:smoke") != 0)
    return fail("host thread attach");
  g_host_api.detach_host_thread(&ctx);
  g_host_api.request_restart(&ctx);
  if (!ps4_gow3gpu_take_restart_request() || ps4_gow3gpu_take_restart_request())
    return fail("restart request latch");
  if (ps4_gow3gpu_dispatch_fault((void *)0x1234, (void *)0x5678) != 7 ||
      ps4_gow3gpu_dispatch_fault(NULL, NULL) != 0)
    return fail("GPU fault dispatch");

  /* Exercise the actual GuestContext register layout and guest-VA arrays used
   * by shim_sceGnmSubmitCommandBuffers, with the candidate's raw HLE callback
   * replaced by an ABI-checking test function. */
  const uint64_t dcb_guest_va = 0x31000;
  const uint64_t ccb_guest_va = 0x31100;
  const uint32_t nop_packet[2] = {0xc0001000u, 0};
  const uint64_t dcb_address_array[1] = {dcb_guest_va};
  const uint64_t ccb_address_array[1] = {ccb_guest_va};
  const uint32_t buffer_size_array[1] = {sizeof(nop_packet)};
  memcpy(memory + 0x30000, dcb_address_array, sizeof(dcb_address_array));
  memcpy(memory + 0x30020, buffer_size_array, sizeof(buffer_size_array));
  memcpy(memory + 0x30030, ccb_address_array, sizeof(ccb_address_array));
  memcpy(memory + 0x30040, buffer_size_array, sizeof(buffer_size_array));
  memcpy(memory + dcb_guest_va, nop_packet, sizeof(nop_packet));
  memcpy(memory + ccb_guest_va, nop_packet, sizeof(nop_packet));
  ctx.rdi = 1;
  ctx.rsi = 0x30000;
  ctx.rdx = 0x30020;
  ctx.rcx = 0x30030;
  ctx.r8 = 0x30040;
  if (ps4_gow3gpu_submit_from_context(&ctx, &g_memory_api, fake_submit) != 0 ||
      g_submit_calls != 1 || ps4_gow3gpu_retained_command_bytes() != 16)
    return fail("GuestContext GNM submit marshalling");
  memset(memory + dcb_guest_va, 0, sizeof(nop_packet));
  memset(memory + ccb_guest_va, 0, sizeof(nop_packet));
  if (g_submitted_dcb[0] != 0xc0001000u || g_submitted_ccb[0] != 0xc0001000u)
    return fail("submitted PM4 copy lifetime");
  if (ps4_gow3gpu_submit_done(fake_submit_done, fake_wait_idle) != 0 ||
      g_submit_done_calls != 1 || ps4_gow3gpu_retained_command_bytes() != 0)
    return fail("submit-done retained-buffer release");

  ctx.rsi = 0x31ffc; /* address array would cross into the uncommitted tail */
  if (ps4_gow3gpu_submit_from_context(&ctx, &g_memory_api, fake_submit) == 0 ||
      g_submit_calls != 1)
    return fail("reject unmapped GNM address array");

  pthread_mutex_destroy(&ctx.vm_mutex);
  munmap(memory, memory_size);
  puts("{\"primary_guest_memory_adapter\":\"PASS\",\"reentrant_guest_memory_lease\":\"PASS\",\"host_runtime_adapter\":\"PASS\",\"eop_and_flip_forwarding\":\"PASS\",\"guest_context_gnm_marshalling\":\"PASS\",\"retained_pm4_lifetime\":\"PASS\",\"unmapped_submit_rejected\":\"PASS\",\"m2_page_protection\":\"PASS\",\"verdict\":\"PASS\"}");
  return 0;
}
