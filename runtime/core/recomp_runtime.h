// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RECOMP_RUNTIME_H
#define RECOMP_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef RECOMP_STANDALONE_REG
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GuestContext GuestContext;
void recomp_register_fn(uint64_t vaddr, void (*fn)(GuestContext *ctx));
void recomp_unimplemented_shim(GuestContext *ctx, const char *shim_name);
void recomp_register_unresolved(uint64_t addr, const char *nid, const char *sym_name, const char *lib_name);
#ifdef __cplusplus
}
#endif
#else

#include "recomp_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runtime initialization and memory layout
GuestContext *recomp_init_runtime(size_t guest_mem_sz, const uint8_t *elf_image, size_t image_size, const char *prog_name);
GuestContext *recomp_init_runtime_file(const char *image_filename, size_t requested_mem_sz, const char *prog_name);
void recomp_init_process_param(GuestContext *ctx, uint64_t proc_param_addr);
void recomp_init_tls(GuestContext *ctx, uint64_t tls_vaddr, uint64_t tls_filesz, uint64_t tls_memsz, uint64_t tls_align);
uint64_t recomp_tls_block_offset(const GuestContext *ctx);
// TLS of loaded PRX modules (libSceFios2, libc) lives below the eboot's static block, in
// RECOMP_MODULE_TLS_SLOTS slots of RECOMP_MODULE_TLS_SLOT_SIZE bytes (see __tls_get_addr).
#define RECOMP_MODULE_TLS_SLOT_SIZE 0x800ULL
#define RECOMP_MODULE_TLS_SLOTS 4ULL
#define RECOMP_MODULE_TLS_RESERVE (RECOMP_MODULE_TLS_SLOT_SIZE * RECOMP_MODULE_TLS_SLOTS)
void recomp_setup_entry_args(GuestContext *ctx, int argc, char **argv, uint64_t entry_addr);
void recomp_dispatch_external(GuestContext *ctx, uint64_t target);
bool recomp_pc_in_guest_code(uint64_t pc);
// Guest accesses crossing 16 bytes (tso_crossing.c): handles the SIGBUS of one, starts the log.
bool tso_crossing_fixup(void *ucontext);
void tso_crossing_start_report(void);
void shim_guest_nonlocal_jump(GuestContext *ctx);
void shim_guest_cxa_throw(GuestContext *ctx);
void recomp_free_runtime(GuestContext *ctx);
GuestContext *recomp_create_thread_context(GuestContext *parent, uint64_t stack_size);
uint64_t recomp_vm_alloc(GuestContext *ctx, size_t size);
uint64_t recomp_vm_alloc_named(GuestContext *ctx, size_t size, int prot, int flags, const char *name);
uint64_t recomp_vm_alloc_named_aligned(GuestContext *ctx, size_t size, size_t alignment, int prot, int flags, const char *name);
uint64_t recomp_vm_alloc_fixed(GuestContext *ctx, uint64_t desired_addr, size_t size, int prot, int flags, const char *name);
int recomp_vm_free(GuestContext *ctx, uint64_t addr, size_t size);
bool recomp_vm_split_extent_at(GuestContext *proc, uint64_t at);

// A guest-memory lease holds the direct-memory and VM extent locks until release.
// The returned host pointer is valid only while the lease remains held. Locks are
// acquired in direct-memory-then-VM order, matching alias map/unmap operations.
typedef struct RecompGuestMemoryLease {
  GuestContext *process;
  void *host_pointer;
  uint64_t guest_addr;
  size_t size;
  bool held;
} RecompGuestMemoryLease;

bool recomp_guest_memory_acquire(GuestContext *ctx, uint64_t guest_addr,
                                 size_t size, int access,
                                 RecompGuestMemoryLease *lease);
// GPU access validates that guest memory is committed and within the process
// mapping, but intentionally ignores the guest CPU's logical page permissions.
bool recomp_guest_gpu_memory_acquire(GuestContext *ctx, uint64_t guest_addr,
                                     size_t size, int access,
                                     RecompGuestMemoryLease *lease);
void recomp_guest_memory_release(RecompGuestMemoryLease *lease);
// The calling thread's lease locks, dropped for a renderer page-fault handler that
// interrupted a lease copy and taken again afterwards (see recomp_guest_memory.c).
typedef struct RecompGuestMemoryLockState {
  GuestContext *process;
  unsigned depth;
} RecompGuestMemoryLockState;
// Host syscalls (read, pread, AIO, file mmap) write guest memory without the guest CPU, so
// renderer page tracking never sees them. Call before the syscall (a tracked page is
// write-protected and a syscall cannot fault through it) and after it (copies the renderer
// made meanwhile must see the new bytes).
void recomp_notify_host_write(uint64_t guest_addr, size_t size);
RecompGuestMemoryLockState recomp_guest_memory_suspend_thread_lock(void);
void recomp_guest_memory_resume_thread_lock(RecompGuestMemoryLockState state);
bool recomp_guest_memory_read(GuestContext *ctx, uint64_t guest_addr,
                              void *destination, size_t size);
bool recomp_guest_memory_write(GuestContext *ctx, uint64_t guest_addr,
                               const void *source, size_t size);
// Called with vm_mutex held whenever an extent becomes committed or free: GPU
// reads cache the extent they validated until the next change.
void recomp_vm_note_change(void);
// Host pointer for a GPU read of [guest_addr, +size) in committed guest memory,
// usually without the VM locks (see recomp_guest_gpu_memory_read).
bool recomp_guest_gpu_fast_range(GuestContext *ctx, uint64_t guest_addr,
                                 size_t size, uint8_t **host);
bool recomp_guest_gpu_memory_read(GuestContext *ctx, uint64_t guest_addr,
                                  void *destination, size_t size);
bool recomp_guest_gpu_memory_write(GuestContext *ctx, uint64_t guest_addr,
                                   const void *source, size_t size);
bool recomp_vm_query_extent(GuestContext *ctx, uint64_t addr,
                            GuestVMExtent *snapshot);
bool recomp_guest_memory_query_region(GuestContext *ctx, uint64_t addr,
                                     uint64_t *region_start,
                                     uint64_t *region_end, int *mapped);
bool recomp_vm_set_protection(GuestContext *ctx, uint64_t addr, size_t size,
                              int prot);

typedef struct RecompUnresolvedSym {
    uint64_t addr;
    char *nid;
    char *sym_name;
    char *lib_name;
    struct RecompUnresolvedSym *next;
} RecompUnresolvedSym;

void recomp_register_unresolved(uint64_t addr, const char *nid, const char *sym_name, const char *lib_name);
const RecompUnresolvedSym *recomp_lookup_unresolved(uint64_t addr);
void recomp_unimplemented_shim(GuestContext *ctx, const char *shim_name);
void shim_unresolved_stub(GuestContext *ctx);
void recomp_unsupported_instruction(uint64_t rip, uint64_t fn_entry, const char *inst_str);

void recomp_init_main_thread(GuestContext *ctx);
GuestContext *recomp_get_current_ctx(void);
void recomp_set_current_ctx(GuestContext *ctx);

void recomp_free_thread_context(GuestContext *ctx);
void ps4_direct_mem_destroy(void);
void ps4_event_flag_destroy(void);
void ps4_sync_destroy(void);
void ps4_videoout_destroy(void);
void ps4_equeue_destroy(void);
void ps4_metal_screen_destroy(void);
void ps4_host_run_main_loop(void);

void ps4_host_quit(int code) __attribute__((noreturn));
void ps4_host_settings_init(void);
void ps4_stall_watchdog_note_present(void);
int ps4_host_setting_pro_mode(void);
void ps4_keyboard_destroy(void);
void ps4_vfs_destroy(void);
void ps4_aio_destroy(void);
void ps4_gnmdriver_destroy(void);
void ps4_ajm_destroy(void);

xmm_reg_t recomp_pclmulqdq(uint64_t a, uint64_t b);
xmm_reg_t recomp_vaesenc(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesenclast(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesdec(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesdeclast(xmm_reg_t s1, xmm_reg_t s2);
xmm_reg_t recomp_vaesimc(xmm_reg_t s);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_STANDALONE_REG

#endif // RECOMP_RUNTIME_H
