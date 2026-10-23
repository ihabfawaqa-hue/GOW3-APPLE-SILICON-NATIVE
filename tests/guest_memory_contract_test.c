#define _DARWIN_C_SOURCE
#include "recomp_runtime.h"
#include "ps4_direct_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t direct_access_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned cpu_write_notifications;

void ps4_direct_mem_guest_access_lock(void) {
  pthread_mutex_lock(&direct_access_mutex);
}

void ps4_direct_mem_guest_access_unlock(void) {
  pthread_mutex_unlock(&direct_access_mutex);
}

void ps4_direct_mem_on_unmap_locked(GuestContext *ctx, uint64_t addr,
                                    size_t size) {
  (void)ctx;
  (void)addr;
  (void)size;
}

void ps4_gnm_notify_cpu_write(uint64_t addr, uint64_t size) {
  (void)addr;
  (void)size;
  ++cpu_write_notifications;
}

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static GuestVMExtent *append_extent(GuestContext *ctx, uint64_t addr,
                                   size_t size, bool is_free, int prot,
                                   GuestVMExtent *previous) {
  GuestVMExtent *extent = calloc(1, sizeof(*extent));
  if (!extent)
    return NULL;
  extent->addr = addr;
  extent->size = size;
  extent->is_free = is_free;
  extent->prot = prot;
  extent->prev = previous;
  if (previous)
    previous->next = extent;
  else
    ctx->vm_extents = extent;
  return extent;
}

static void free_extents(GuestContext *ctx) {
  GuestVMExtent *extent = ctx->vm_extents;
  while (extent) {
    GuestVMExtent *next = extent->next;
    free(extent);
    extent = next;
  }
}

int main(void) {
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const size_t memory_size = page * 8;
  GuestContext ctx = {0};
  ctx.mem_base = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
  CHECK(ctx.mem_base != MAP_FAILED);
  ctx.mem_size = memory_size;
  ctx.heap_ptr = page;
  ctx.process_ctx = &ctx;
  CHECK(pthread_mutex_init(&ctx.vm_mutex, NULL) == 0);

  GuestVMExtent *extent = append_extent(&ctx, page, page, false,
                                        PROT_READ | PROT_WRITE, NULL);
  CHECK(extent);
  extent = append_extent(&ctx, page * 2, page, false, PROT_READ, extent);
  CHECK(extent);
  extent = append_extent(&ctx, page * 3, page, true,
                         PROT_READ | PROT_WRITE, extent);
  CHECK(extent);
  extent = append_extent(&ctx, page * 4, page, false,
                         PROT_READ | PROT_WRITE, extent);
  CHECK(extent);
  extent = append_extent(&ctx, page * 5, page, false, PROT_READ, extent);
  CHECK(extent);
  extent = append_extent(&ctx, page * 6, page, false, PROT_READ, extent);
  CHECK(extent);

  GuestVMExtent *image_extent = calloc(1, sizeof(*image_extent));
  CHECK(image_extent);
  image_extent->addr = 0;
  image_extent->size = page;
  image_extent->prot = PROT_READ | PROT_WRITE;
  image_extent->is_pinned = true;
  strcpy(image_extent->name, "image");
  image_extent->next = ctx.vm_extents;
  ctx.vm_extents->prev = image_extent;
  ctx.vm_extents = image_extent;

  // Map two guest VAs to the same backing page, as the direct-memory layer does.
  char shm_name[64];
  snprintf(shm_name, sizeof(shm_name), "/gow3_guest_memory_%ld", (long)getpid());
  int shared_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(shared_fd >= 0);
  CHECK(shm_unlink(shm_name) == 0);
  CHECK(ftruncate(shared_fd, (off_t)page) == 0);
  void *alias_a = mmap(ctx.mem_base + page, page, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, shared_fd, 0);
  void *alias_b = mmap(ctx.mem_base + page * 4, page, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, shared_fd, 0);
  close(shared_fd);
  CHECK(alias_a == ctx.mem_base + page);
  CHECK(alias_b == ctx.mem_base + page * 4);

  // Initial image/TLS/arguments live before the first managed VM extent.
  RecompGuestMemoryLease lease;
  CHECK(recomp_guest_memory_acquire(&ctx, 64, 8, PROT_READ | PROT_WRITE,
                                    &lease));
  CHECK(lease.host_pointer == ctx.mem_base + 64 && lease.held);
  CHECK(pthread_mutex_trylock(&ctx.vm_mutex) == EBUSY);
  CHECK(pthread_mutex_trylock(&direct_access_mutex) == EBUSY);
  recomp_guest_memory_release(&lease);
  CHECK(!lease.held && lease.host_pointer == NULL);
  CHECK(recomp_vm_set_protection(&ctx, 64, 8, PROT_READ));
  CHECK(!recomp_guest_memory_acquire(&ctx, 64, 8, PROT_WRITE, &lease));
  CHECK(recomp_guest_memory_acquire(&ctx, 64, 8, PROT_READ, &lease));
  recomp_guest_memory_release(&lease);
  CHECK(recomp_vm_set_protection(&ctx, 64, 8, PROT_READ | PROT_WRITE));

  // Committed, free, and read-only ranges are distinguished across the whole span.
  CHECK(recomp_guest_memory_acquire(&ctx, page + 8, 16, PROT_WRITE, &lease));
  recomp_guest_memory_release(&lease);
  CHECK(recomp_guest_memory_acquire(&ctx, page * 2 + 8, 16, PROT_READ, &lease));
  recomp_guest_memory_release(&lease);
  CHECK(!recomp_guest_memory_acquire(&ctx, page * 2 + 8, 16, PROT_WRITE, &lease));
  CHECK(!recomp_guest_memory_acquire(&ctx, page * 3 + 8, 16, PROT_READ, &lease));
  CHECK(!recomp_guest_memory_acquire(&ctx, page * 7 + 8, 16, PROT_READ, &lease));

  // Adjacent extents can jointly satisfy a range; their permissions are checked.
  CHECK(recomp_guest_memory_acquire(&ctx, page * 5, page * 2, PROT_READ, &lease));
  recomp_guest_memory_release(&lease);
  CHECK(!recomp_guest_memory_acquire(&ctx, page * 5, page * 2, PROT_WRITE, &lease));
  CHECK(recomp_vm_set_protection(&ctx, page * 5, page * 2,
                                 PROT_READ | PROT_WRITE));
  CHECK(recomp_guest_memory_acquire(&ctx, page * 5, page * 2, PROT_WRITE, &lease));
  recomp_guest_memory_release(&lease);

  // Protection updates are split and observed under the same VM lock.
  CHECK(recomp_vm_set_protection(&ctx, page * 2, page, PROT_NONE));
  GuestVMExtent snapshot;
  CHECK(recomp_vm_query_extent(&ctx, page * 2 + 16, &snapshot));
  CHECK(!snapshot.is_free && snapshot.prot == PROT_NONE);
  CHECK(snapshot.prev == NULL && snapshot.next == NULL);
  CHECK(!recomp_guest_memory_acquire(&ctx, page * 2 + 8, 16, PROT_READ, &lease));
  CHECK(recomp_vm_set_protection(&ctx, page * 2, page, PROT_READ));

  // Read/write helpers preserve aliasing and invalidate renderer-side copies.
  const uint32_t value = 0x53a71c29;
  uint32_t observed = 0;
  CHECK(recomp_guest_memory_write(&ctx, page + 64, &value, sizeof(value)));
  CHECK(recomp_guest_memory_read(&ctx, page * 4 + 64, &observed, sizeof(observed)));
  CHECK(observed == value);
  CHECK(cpu_write_notifications == 1);

  // Reject null, zero-length, unmapped, read-only writes, and overflowing spans.
  CHECK(!recomp_guest_memory_acquire(&ctx, 0, 4, PROT_READ, &lease));
  CHECK(!recomp_guest_memory_acquire(&ctx, page, 0, PROT_READ, &lease));
  CHECK(!recomp_guest_memory_acquire(&ctx, UINT64_MAX - 3, 8, PROT_READ, &lease));
  CHECK(!recomp_guest_memory_acquire(&ctx, memory_size - 4, 8, PROT_READ, &lease));
  CHECK(!recomp_guest_memory_acquire(&ctx, page * 2, 4, PROT_WRITE, &lease));
  CHECK(!recomp_vm_query_extent(&ctx, page * 7, &snapshot));

  free_extents(&ctx);
  pthread_mutex_destroy(&ctx.vm_mutex);
  munmap(ctx.mem_base, memory_size);
  puts("guest-memory-contract: PASS (translation, permissions, aliases, leases, overflow)");
  return 0;
}
