// SPDX-License-Identifier: GPL-2.0-or-later
#include "recomp_runtime.h"
#include "ps4_direct_mem.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

// CPU writes made through this API invalidate any renderer-side copies. The
// direct-memory module supplies the weak hook used by the current Metal path.
extern void ps4_gnm_notify_cpu_write(uint64_t addr, uint64_t size)
    __attribute__((weak));

static GuestContext *process_context(GuestContext *ctx) {
  return ctx && ctx->process_ctx ? ctx->process_ctx : ctx;
}

// GPU page tracking deliberately uses host protection faults. A fault can occur
// while one of these callbacks is copying guest memory; the signal handler may
// re-enter the callback on the same thread. Keep the VM/direct-memory lease
// re-entrant for that thread while still excluding remaps and frees elsewhere.
static _Thread_local GuestContext *g_memory_lock_process;
static _Thread_local unsigned g_memory_lock_depth;

static bool memory_lock(GuestContext *proc) {
  if (!proc)
    return false;
  if (g_memory_lock_depth) {
    if (g_memory_lock_process != proc || g_memory_lock_depth == UINT_MAX)
      return false;
    ++g_memory_lock_depth;
    return true;
  }
  ps4_direct_mem_guest_access_lock();
  pthread_mutex_lock(&proc->vm_mutex);
  g_memory_lock_process = proc;
  g_memory_lock_depth = 1;
  return true;
}

static void memory_unlock(GuestContext *proc) {
  if (!proc || !g_memory_lock_depth || g_memory_lock_process != proc)
    return;
  if (--g_memory_lock_depth)
    return;
  g_memory_lock_process = NULL;
  pthread_mutex_unlock(&proc->vm_mutex);
  ps4_direct_mem_guest_access_unlock();
}

static bool range_is_accessible_locked(GuestContext *proc, uint64_t start,
                                       uint64_t end, int access,
                                       bool require_guest_access) {
  uint64_t cursor = start;

  while (cursor < end) {
    GuestVMExtent *extent = proc->vm_extents;
    while (extent && !(cursor >= extent->addr &&
                       cursor - extent->addr < extent->size))
      extent = extent->next;
    if (!extent || extent->is_free || extent->size == 0)
      return false;
    if (require_guest_access && (extent->prot & access) != access)
      return false;
    if (extent->size > UINT64_MAX - extent->addr)
      return false;

    const uint64_t extent_end = extent->addr + extent->size;
    if (extent_end <= cursor)
      return false;
    cursor = end < extent_end ? end : extent_end;
  }
  return true;
}

// Splits the non-free extent that strictly contains `at` into [addr, at) and
// [at, end); both halves retain all metadata. Caller holds vm_mutex.
bool recomp_vm_split_extent_at(GuestContext *proc, uint64_t at) {
  if (!proc)
    return false;
  for (GuestVMExtent *extent = proc->vm_extents; extent;
       extent = extent->next) {
    if (extent->is_free || at <= extent->addr ||
        at - extent->addr >= extent->size)
      continue;
    if (extent->size > UINT64_MAX - extent->addr)
      return false;
    GuestVMExtent *tail = (GuestVMExtent *)calloc(1, sizeof(*tail));
    if (!tail)
      return false;
    *tail = *extent;
    tail->addr = at;
    tail->size = extent->addr + extent->size - at;
    tail->prev = extent;
    extent->size = at - extent->addr;
    extent->next = tail;
    if (tail->next)
      tail->next->prev = tail;
    return true;
  }
  return true;
}

static bool acquire_guest_memory(GuestContext *ctx, uint64_t guest_addr,
                                 size_t size, int access,
                                 bool require_guest_access,
                                 RecompGuestMemoryLease *lease) {
  if (!lease)
    return false;
  memset(lease, 0, sizeof(*lease));

  const int valid_access = PROT_READ | PROT_WRITE | PROT_EXEC;
  if (!ctx || !size || !guest_addr || !access || (access & ~valid_access))
    return false;

  GuestContext *proc = process_context(ctx);
  if (!proc || !proc->mem_base || guest_addr > proc->mem_size ||
      size > proc->mem_size - (size_t)guest_addr)
    return false;

  const uintptr_t base = (uintptr_t)proc->mem_base;
  if (guest_addr > UINTPTR_MAX - base)
    return false;
  const uint64_t end = guest_addr + (uint64_t)size;

  // This order is shared with direct-memory mapping. Holding both locks pins
  // the extent and prevents remapping of a physical alias until release.
  if (!memory_lock(proc))
    return false;
  const bool valid = range_is_accessible_locked(
      proc, guest_addr, end, access, require_guest_access);
  if (!valid) {
    memory_unlock(proc);
    return false;
  }

  lease->process = proc;
  lease->host_pointer = (void *)(base + (uintptr_t)guest_addr);
  lease->guest_addr = guest_addr;
  lease->size = size;
  lease->held = true;
  return true;
}

bool recomp_guest_memory_acquire(GuestContext *ctx, uint64_t guest_addr,
                                 size_t size, int access,
                                 RecompGuestMemoryLease *lease) {
  return acquire_guest_memory(ctx, guest_addr, size, access, true, lease);
}

bool recomp_guest_gpu_memory_acquire(GuestContext *ctx, uint64_t guest_addr,
                                     size_t size, int access,
                                     RecompGuestMemoryLease *lease) {
  if (guest_addr < 0x10000ULL)
    return false;
  return acquire_guest_memory(ctx, guest_addr, size, access, false, lease);
}

void recomp_guest_memory_release(RecompGuestMemoryLease *lease) {
  if (!lease || !lease->held || !lease->process)
    return;
  GuestContext *proc = lease->process;
  lease->held = false;
  lease->process = NULL;
  lease->host_pointer = NULL;
  lease->guest_addr = 0;
  lease->size = 0;
  memory_unlock(proc);
}

bool recomp_guest_memory_read(GuestContext *ctx, uint64_t guest_addr,
                              void *destination, size_t size) {
  if (!destination)
    return false;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_memory_acquire(ctx, guest_addr, size, PROT_READ, &lease))
    return false;
  memcpy(destination, lease.host_pointer, size);
  recomp_guest_memory_release(&lease);
  return true;
}

bool recomp_guest_memory_write(GuestContext *ctx, uint64_t guest_addr,
                               const void *source, size_t size) {
  if (!source)
    return false;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_memory_acquire(ctx, guest_addr, size, PROT_WRITE, &lease))
    return false;
  memcpy(lease.host_pointer, source, size);
  recomp_guest_memory_release(&lease);
  if (ps4_gnm_notify_cpu_write)
    ps4_gnm_notify_cpu_write(guest_addr, size);
  return true;
}

bool recomp_guest_gpu_memory_read(GuestContext *ctx, uint64_t guest_addr,
                                  void *destination, size_t size) {
  if (!destination)
    return false;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_gpu_memory_acquire(ctx, guest_addr, size, PROT_READ,
                                       &lease))
    return false;
  memcpy(destination, lease.host_pointer, size);
  recomp_guest_memory_release(&lease);
  return true;
}

bool recomp_guest_gpu_memory_write(GuestContext *ctx, uint64_t guest_addr,
                                   const void *source, size_t size) {
  if (!source)
    return false;
  RecompGuestMemoryLease lease;
  if (!recomp_guest_gpu_memory_acquire(ctx, guest_addr, size, PROT_WRITE,
                                       &lease))
    return false;
  memmove(lease.host_pointer, source, size);
  recomp_guest_memory_release(&lease);
  // GPU writes have separate completion and cache-visibility rules. Do not
  // report them as CPU writes through ps4_gnm_notify_cpu_write().
  return true;
}

bool recomp_vm_query_extent(GuestContext *ctx, uint64_t addr,
                            GuestVMExtent *snapshot) {
  if (!ctx || !snapshot)
    return false;
  GuestContext *proc = process_context(ctx);
  if (!proc)
    return false;

  if (!memory_lock(proc))
    return false;
  bool found = false;
  for (GuestVMExtent *extent = proc->vm_extents; extent; extent = extent->next) {
    if (addr >= extent->addr && addr - extent->addr < extent->size) {
      *snapshot = *extent;
      snapshot->prev = NULL;
      snapshot->next = NULL;
      found = true;
      break;
    }
  }
  memory_unlock(proc);
  return found;
}

bool recomp_guest_memory_query_region(GuestContext *ctx, uint64_t addr,
                                     uint64_t *region_start,
                                     uint64_t *region_end, int *mapped) {
  GuestContext *proc = process_context(ctx);
  if (!proc || !proc->mem_base || !region_start || !region_end || !mapped ||
      addr >= proc->mem_size || !memory_lock(proc))
    return false;

  const uint64_t zero_end = proc->mem_size < 0x10000ULL
                                ? proc->mem_size
                                : 0x10000ULL;
  if (addr < zero_end) {
    *region_start = 0;
    *region_end = zero_end;
    *mapped = 0;
    memory_unlock(proc);
    return true;
  }

  uint64_t cursor = zero_end;
  for (GuestVMExtent *extent = proc->vm_extents; extent;
       extent = extent->next) {
    if (!extent->size || extent->addr >= proc->mem_size ||
        extent->size > UINT64_MAX - extent->addr)
      continue;
    uint64_t end = extent->addr + extent->size;
    if (end > proc->mem_size)
      end = proc->mem_size;
    uint64_t start = extent->addr < zero_end ? zero_end : extent->addr;
    if (end <= start || end <= cursor)
      continue;
    if (start < cursor)
      start = cursor;
    if (addr < start) {
      *region_start = cursor;
      *region_end = start;
      *mapped = 0;
      memory_unlock(proc);
      return true;
    }
    if (addr < end) {
      *region_start = start;
      *region_end = end;
      *mapped = !extent->is_free;
      memory_unlock(proc);
      return true;
    }
    cursor = end;
  }
  *region_start = cursor;
  *region_end = proc->mem_size;
  *mapped = 0;
  const bool valid = *region_start <= addr && addr < *region_end;
  memory_unlock(proc);
  return valid;
}

bool recomp_vm_set_protection(GuestContext *ctx, uint64_t addr, size_t size,
                              int prot) {
  if (!ctx || !size || !addr ||
      (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
    return false;
  GuestContext *proc = process_context(ctx);
  if (!proc || addr > proc->mem_size || size > proc->mem_size - (size_t)addr)
    return false;

  const uint64_t end_unaligned = addr + (uint64_t)size;
  if (end_unaligned > UINT64_MAX - 4095ULL)
    return false;
  const uint64_t start = addr & ~4095ULL;
  const uint64_t end = (end_unaligned + 4095ULL) & ~4095ULL;

  if (!memory_lock(proc))
    return false;
  if (!recomp_vm_split_extent_at(proc, start) ||
      !recomp_vm_split_extent_at(proc, end)) {
    memory_unlock(proc);
    return false;
  }

  uint64_t cursor = start;
  while (cursor < end) {
    GuestVMExtent *extent = proc->vm_extents;
    while (extent && !(cursor >= extent->addr &&
                       cursor - extent->addr < extent->size))
      extent = extent->next;
    if (!extent || extent->is_free || !extent->size) {
      memory_unlock(proc);
      return false;
    }
    if (extent->size > UINT64_MAX - extent->addr) {
      memory_unlock(proc);
      return false;
    }
    const uint64_t extent_end = extent->addr + extent->size;
    if (extent_end <= cursor) {
      memory_unlock(proc);
      return false;
    }
    cursor = end < extent_end ? end : extent_end;
  }

  for (GuestVMExtent *extent = proc->vm_extents; extent; extent = extent->next) {
    if (!extent->is_free && extent->addr >= start &&
        extent->addr + extent->size <= end)
      extent->prot = prot;
  }
  memory_unlock(proc);
  return true;
}
