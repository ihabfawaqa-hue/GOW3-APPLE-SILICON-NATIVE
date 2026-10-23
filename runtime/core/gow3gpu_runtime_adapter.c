// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3gpu_runtime_adapter.h"
#include "recomp_runtime.h"

#include "ps4_kernel_sys.h"
#include "../kernel/memory/ps4_direct_mem.h"
#include "../kernel/vfs/ps4_vfs.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <pthread.h>
#endif

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
               "GPU signal dispatch requires lock-free function-pointer atomics");

typedef struct Gow3GpuMapHookState {
  Gow3GpuGuestRangeHook map;
  Gow3GpuGuestRangeHook unmap;
  Gow3GpuGuestRangeHook invalidate;
  void *hook_context;
  int installed;
} Gow3GpuMapHookState;

typedef struct Gow3GpuMapRange {
  uint64_t address;
  uint64_t size;
} Gow3GpuMapRange;

static pthread_mutex_t g_gow3gpu_hook_mutex = PTHREAD_MUTEX_INITIALIZER;
static Gow3GpuMapHookState g_gow3gpu_hooks;
static _Atomic(Ps4Gow3GpuFaultHandler) g_gow3gpu_fault_handler;
static atomic_int g_gow3gpu_restart_requested;
static atomic_int g_gow3gpu_runtime_api_installed;
static __thread char g_gow3gpu_saved_thread_name[64];
static __thread int g_gow3gpu_saved_thread_name_valid;

static GuestContext *process_context(void *user_context) {
  GuestContext *ctx = (GuestContext *)user_context;
  if (!ctx) return NULL;
  return ctx->process_ctx ? ctx->process_ctx : ctx;
}

static int guest_query_region(void *user_context, uint64_t address,
                              uint64_t *region_start, uint64_t *region_end,
                              int *mapped) {
  GuestContext *ctx = process_context((GuestContext *)user_context);
  return ctx && recomp_guest_memory_query_region(ctx, address, region_start,
                                                 region_end, mapped);
}

static int guest_is_mapped(void *user_context, uint64_t address, uint64_t size) {
  GuestContext *ctx = process_context((GuestContext *)user_context);
  if (!ctx || !ctx->mem_base || address < 0x10000ULL ||
      address >= ctx->mem_size || size > SIZE_MAX)
    return 0;
  // The pinned GPU source uses size=0 for point membership checks.
  if (!size)
    size = 1;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_gpu_memory_acquire(ctx, address, (size_t)size, PROT_READ,
                                      &lease))
    return 0;
  recomp_guest_memory_release(&lease);
  return 1;
}

static int guest_host_to_guest(void *user_context, const void *host_pointer,
                               uint64_t *guest_address) {
  GuestContext *ctx = process_context(user_context);
  if (!ctx || !ctx->mem_base || !host_pointer || !guest_address) return 0;
  const uintptr_t base = (uintptr_t)ctx->mem_base;
  const uintptr_t pointer = (uintptr_t)host_pointer;
  if (pointer < base || pointer - base >= ctx->mem_size) return 0;
  const uint64_t address = (uint64_t)(pointer - base);
  if (address < 0x10000ULL || address >= ctx->mem_size) return 0;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_gpu_memory_acquire(ctx, address, 1, PROT_READ, &lease))
    return 0;
  recomp_guest_memory_release(&lease);
  *guest_address = address;
  return 1;
}

static int guest_read(void *user_context, uint64_t address, void *destination,
                      uint64_t size) {
  GuestContext *ctx = process_context(user_context);
  if (!destination || size > SIZE_MAX) return 0;
  return recomp_guest_gpu_memory_read(ctx, address, destination,
                                      (size_t)size);
}

static int guest_atomic_load(void *user_context, uint64_t address, uint32_t size,
                             uint64_t *value) {
  GuestContext *ctx = process_context(user_context);
  if (!value || (size != 1 && size != 4 && size != 8) || address % size ||
      !ctx)
    return 0;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_gpu_memory_acquire(ctx, address, size, PROT_READ, &lease))
    return 0;
  const void *p = lease.host_pointer;
  switch (size) {
  case 1:
    *value = __atomic_load_n((const uint8_t *)p, __ATOMIC_ACQUIRE);
    break;
  case 4:
    *value = __atomic_load_n((const uint32_t *)p, __ATOMIC_ACQUIRE);
    break;
  default:
    *value = __atomic_load_n((const uint64_t *)p, __ATOMIC_ACQUIRE);
    break;
  }
  recomp_guest_memory_release(&lease);
  return 1;
}

static int guest_atomic_u64(void *user_context, uint64_t address,
                           uint32_t operation, uint64_t operand,
                           uint64_t *previous_value) {
  GuestContext *ctx = process_context(user_context);
  if (!previous_value || (address & 7) || !ctx)
    return 0;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_gpu_memory_acquire(
          ctx, address, sizeof(uint64_t), PROT_READ | PROT_WRITE, &lease))
    return 0;
  uint64_t *p = (uint64_t *)lease.host_pointer;
  int success = 1;
  switch (operation) {
  case GOW3GPU_ATOMIC_FETCH_ADD:
    *previous_value = __atomic_fetch_add(p, operand, __ATOMIC_ACQ_REL);
    break;
  case GOW3GPU_ATOMIC_FETCH_SUB:
    *previous_value = __atomic_fetch_sub(p, operand, __ATOMIC_ACQ_REL);
    break;
  case GOW3GPU_ATOMIC_FETCH_SUB_IF_NONZERO: {
    uint64_t current = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    do {
      if (!current) {
        *previous_value = 0;
        break;
      }
    } while (!__atomic_compare_exchange_n(p, &current, current - operand, 1,
                                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    if (current)
      *previous_value = current;
    break;
  }
  case GOW3GPU_ATOMIC_STORE_RELEASE:
    *previous_value = __atomic_exchange_n(p, operand, __ATOMIC_RELEASE);
    break;
  default:
    success = 0;
    break;
  }
  recomp_guest_memory_release(&lease);
  return success;
}

static int guest_writeback(void *user_context, uint64_t address,
                           const void *source, uint64_t size) {
  GuestContext *ctx = process_context(user_context);
  if (!source || size > SIZE_MAX) return 0;
  return recomp_guest_gpu_memory_write(ctx, address, source, (size_t)size);
}

static uint64_t guest_clamp_range(void *user_context, uint64_t address,
                                  uint64_t requested_size) {
  GuestContext *ctx = process_context(user_context);
  if (!ctx || !ctx->mem_base || address < 0x10000ULL || address >= ctx->mem_size)
    return 0;
  const uint64_t available = (uint64_t)ctx->mem_size - address;
  return requested_size < available ? requested_size : available;
}

static int guest_gpu_protect(void *user_context, uint64_t address, uint64_t size,
                             int read, int write) {
  GuestContext *ctx = process_context(user_context);
  if (size == 0) return 1;
  if (!ctx || !ctx->mem_base || (write && !read) || size > SIZE_MAX ||
      address < 0x10000ULL || address >= ctx->mem_size ||
      size > (uint64_t)ctx->mem_size - address)
    return 0;
  const long page_size_signed = sysconf(_SC_PAGESIZE);
  if (page_size_signed <= 0) return 0;
  const uint64_t page_size = (uint64_t)page_size_signed;
  if ((page_size & (page_size - 1)) != 0) return 0;
  const uint64_t start = address & ~(page_size - 1);
  const uint64_t end_unaligned = address + size;
  if (end_unaligned > UINT64_MAX - (page_size - 1)) return 0;
  const uint64_t end = (end_unaligned + page_size - 1) & ~(page_size - 1);
  if (start < 0x10000ULL || end > ctx->mem_size || end <= start) return 0;
  const int protection = read ? (PROT_READ | (write ? PROT_WRITE : 0)) : PROT_NONE;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_gpu_memory_acquire(ctx, start, (size_t)(end - start),
                                       PROT_READ, &lease))
    return 0;
  const int result = mprotect(lease.host_pointer, (size_t)(end - start),
                              protection);
  recomp_guest_memory_release(&lease);
  return result == 0;
}

static int install_gpu_hooks(void *user_context, Gow3GpuGuestRangeHook map,
                             Gow3GpuGuestRangeHook unmap,
                             Gow3GpuGuestRangeHook invalidate,
                             void *hook_context) {
  GuestContext *process = process_context(user_context);
  if (!process || !map || !unmap || !invalidate ||
      process->heap_ptr > process->mem_size)
    return 0;
  pthread_mutex_lock(&g_gow3gpu_hook_mutex);
  if (g_gow3gpu_hooks.installed) {
    pthread_mutex_unlock(&g_gow3gpu_hook_mutex);
    return 0;
  }
  g_gow3gpu_hooks = (Gow3GpuMapHookState){map, unmap, invalidate, hook_context, 1};
  GuestContext *ctx = process_context(user_context);
  size_t range_count = 0;
  ps4_direct_mem_guest_access_lock();
  pthread_mutex_lock(&ctx->vm_mutex);
  for (GuestVMExtent *e = ctx->vm_extents; e; e = e->next)
    if (!e->is_free && e->size && e->addr < ctx->mem_size &&
        e->size <= ctx->mem_size - e->addr &&
        e->addr + e->size > 0x10000ULL)
      range_count++;
  Gow3GpuMapRange *ranges = (Gow3GpuMapRange *)calloc(range_count, sizeof(*ranges));
  if (range_count && !ranges) {
    pthread_mutex_unlock(&ctx->vm_mutex);
    ps4_direct_mem_guest_access_unlock();
    memset(&g_gow3gpu_hooks, 0, sizeof(g_gow3gpu_hooks));
    pthread_mutex_unlock(&g_gow3gpu_hook_mutex);
    return 0;
  }
  size_t count = 0;
  for (GuestVMExtent *e = ctx->vm_extents; e; e = e->next) {
    if (!e->is_free && e->size && e->addr < ctx->mem_size &&
        e->size <= ctx->mem_size - e->addr &&
        e->addr + e->size > 0x10000ULL) {
      const uint64_t start = e->addr < 0x10000ULL ? 0x10000ULL : e->addr;
      ranges[count++] = (Gow3GpuMapRange){start, e->addr + e->size - start};
    }
  }
  pthread_mutex_unlock(&ctx->vm_mutex);
  ps4_direct_mem_guest_access_unlock();
  const Gow3GpuMapHookState hooks = g_gow3gpu_hooks;
  pthread_mutex_unlock(&g_gow3gpu_hook_mutex);
  for (size_t i = 0; i < count; ++i)
    hooks.map(hooks.hook_context, ranges[i].address, ranges[i].size);
  free(ranges);
  return 1;
}

static int32_t *host_errno_address(void *user_context) {
  (void)user_context;
  return (int32_t *)&errno;
}

static uint64_t host_process_time_us(void *user_context) {
  (void)user_context;
  return sceKernelGetProcessTime();
}

static uint64_t host_process_time_counter(void *user_context) {
  (void)user_context;
  return sceKernelGetProcessTimeCounter();
}

static uint64_t host_tsc_frequency(void *user_context) {
  (void)user_context;
  return sceKernelGetTscFrequency();
}

static int host_translate_path(void *user_context, const char *guest_path,
                               char *host_path, uint64_t host_path_size) {
  (void)user_context;
  if (!guest_path || !host_path || host_path_size > SIZE_MAX) return -1;
  return ps4_vfs_resolve(guest_path, host_path, (size_t)host_path_size);
}

static int host_attach_thread(void *user_context, const char *name) {
  (void)user_context;
  if (!g_gow3gpu_saved_thread_name_valid) {
#if defined(__APPLE__)
    g_gow3gpu_saved_thread_name_valid =
        pthread_getname_np(pthread_self(), g_gow3gpu_saved_thread_name,
                           sizeof(g_gow3gpu_saved_thread_name)) == 0;
#endif
  }
  if (!name || !name[0]) return 0;
#if defined(__APPLE__)
  return pthread_setname_np(name);
#else
  return pthread_setname_np(pthread_self(), name);
#endif
}

static void host_detach_thread(void *user_context) {
  (void)user_context;
  if (!g_gow3gpu_saved_thread_name_valid) return;
#if defined(__APPLE__)
  pthread_setname_np(g_gow3gpu_saved_thread_name);
#else
  pthread_setname_np(pthread_self(), g_gow3gpu_saved_thread_name);
#endif
  g_gow3gpu_saved_thread_name_valid = 0;
}

static void host_request_restart(void *user_context) {
  (void)user_context;
  atomic_store_explicit(&g_gow3gpu_restart_requested, 1, memory_order_release);
}

int ps4_gow3gpu_install_runtime_apis(GuestContext *ctx,
                                     Ps4Gow3GpuSetGuestMemoryApi set_memory_api,
                                     Ps4Gow3GpuSetHostRuntimeApi set_host_api,
                                     Ps4Gow3GpuFaultHandler fault_handler) {
  if (!ctx || !ctx->mem_base || !set_memory_api || !set_host_api || !fault_handler)
    return -1;
  GuestContext *proc = process_context(ctx);
  int expected = 0;
  if (!atomic_compare_exchange_strong(&g_gow3gpu_runtime_api_installed, &expected, -1))
    return -2;

  Gow3GpuGuestMemoryApi memory_api = {
      .abi_version = GOW3GPU_GUEST_MEMORY_ABI_VERSION,
      .struct_size = sizeof(Gow3GpuGuestMemoryApi),
      .user_context = proc,
      .is_mapped = guest_is_mapped,
      .host_to_guest = guest_host_to_guest,
      .query_region = guest_query_region,
      .read = guest_read,
      .atomic_load = guest_atomic_load,
      .atomic_u64 = guest_atomic_u64,
      .writeback = guest_writeback,
      .clamp_range_size = guest_clamp_range,
      .gpu_protect = guest_gpu_protect,
      .install_gpu_hooks = install_gpu_hooks,
  };
  Gow3GpuHostRuntimeApi host_api = {
      .abi_version = GOW3GPU_HOST_RUNTIME_ABI_VERSION,
      .struct_size = sizeof(Gow3GpuHostRuntimeApi),
      .user_context = proc,
      .errno_address = host_errno_address,
      .process_time_us = host_process_time_us,
      .process_time_counter = host_process_time_counter,
      .tsc_frequency = host_tsc_frequency,
      .translate_path = host_translate_path,
      .attach_host_thread = host_attach_thread,
      .detach_host_thread = host_detach_thread,
      .request_restart = host_request_restart,
  };

  atomic_store_explicit(&g_gow3gpu_fault_handler, fault_handler, memory_order_release);
  if (set_memory_api(&memory_api) != 0 || set_host_api(&host_api) != 0) {
    atomic_store_explicit(&g_gow3gpu_fault_handler, NULL, memory_order_release);
    /* One side may already have accepted its one-shot API, so this process cannot retry
     * installation safely without a library teardown/reset contract. */
    atomic_store_explicit(&g_gow3gpu_runtime_api_installed, -1, memory_order_release);
    return -3;
  }
  atomic_store_explicit(&g_gow3gpu_runtime_api_installed, 1, memory_order_release);
  return 0;
}

int ps4_gow3gpu_dispatch_fault(void *ucontext, void *address) {
  Ps4Gow3GpuFaultHandler handler =
      atomic_load_explicit(&g_gow3gpu_fault_handler, memory_order_acquire);
  return handler ? handler(ucontext, address) : 0;
}

void ps4_gow3gpu_guest_map(uint64_t address, uint64_t size) {
  pthread_mutex_lock(&g_gow3gpu_hook_mutex);
  const Gow3GpuMapHookState hooks = g_gow3gpu_hooks;
  pthread_mutex_unlock(&g_gow3gpu_hook_mutex);
  if (hooks.installed && hooks.map) hooks.map(hooks.hook_context, address, size);
}

void ps4_gow3gpu_guest_unmap(uint64_t address, uint64_t size) {
  pthread_mutex_lock(&g_gow3gpu_hook_mutex);
  const Gow3GpuMapHookState hooks = g_gow3gpu_hooks;
  pthread_mutex_unlock(&g_gow3gpu_hook_mutex);
  if (hooks.installed && hooks.unmap) hooks.unmap(hooks.hook_context, address, size);
}

void ps4_gow3gpu_guest_invalidate(uint64_t address, uint64_t size) {
  pthread_mutex_lock(&g_gow3gpu_hook_mutex);
  const Gow3GpuMapHookState hooks = g_gow3gpu_hooks;
  pthread_mutex_unlock(&g_gow3gpu_hook_mutex);
  if (hooks.installed && hooks.invalidate)
    hooks.invalidate(hooks.hook_context, address, size);
}

int ps4_gow3gpu_take_restart_request(void) {
  return atomic_exchange_explicit(&g_gow3gpu_restart_requested, 0,
                                 memory_order_acq_rel);
}

/* The direct-memory mapper invokes these after replacing a host mapping. */
void ps4_gnm_guest_remapped(uint64_t address, uint64_t size) {
  ps4_gow3gpu_guest_invalidate(address, size);
}

/* mtypeprotect is bookkeeping in the primary runtime; publish writes to the
 * imported renderer's texture and buffer caches through the same invalidation path. */
void ps4_gnm_notify_cpu_write(uint64_t address, uint64_t size) {
  ps4_gow3gpu_guest_invalidate(address, size);
}
