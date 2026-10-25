// Guest memory accesses that cross a 16-byte boundary.
//
// The recompiler emits every guest load and store of 2, 4 or 8 bytes as
//     add  xS, x16, #size-1 ; eor xS, xS, x16 ; tbz xS, #4, ok   (crosses 16 bytes?)
//     ldr ; dmb ishld        (or dmb ish ; str)                    crossing
//     b done
//   ok: add xS, x28, x16 ; ldapr/stlr [xS]                         common case
// with xS = x7 (or x6 where x7 is in use). The check exists because ldapr/stlr fault on an
// access crossing 16 bytes. tools/peephole_guest_code.py drops the check and keeps only the
// common case, so these accesses fault (SIGBUS). tso_crossing_fixup sends such an access to a
// routine below that does what the crossing case did: the same plain access and barrier,
// then a jump back to the next instruction. Faults are rare and counted; a background thread
// logs the guest addresses of the sites that fault, and listing a site in
// tools/tso_crossing_sites.txt makes the peephole keep its check.
//
// The return address goes in the 16 bytes below the guest code's sp: the Darwin arm64 ABI
// keeps a 128-byte red zone there that the kernel leaves out of the signal frame, and the
// recompiled code never uses it. The routine loads it back into xS, which no recompiled code
// reads after the access (its value differed between the two cases before too).
#include "recomp_runtime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__arm64__)
#include <signal.h>
#include <sys/ucontext.h>

// One 16-byte routine per (scratch register, access, data register): index
// ((scratch == x6) * 6 + access) * 32 + register, register 31 being wzr/xzr.
__asm__(
    ".text\n"
    ".macro GOW3_TSO_LOAD insn, rt, rn\n"
    "  .p2align 4\n"
    "  \\insn \\rt, [\\rn]\n"
    "  dmb ishld\n"
    "  ldr \\rn, [sp], #16\n"
    "  br \\rn\n"
    ".endm\n"
    ".macro GOW3_TSO_STORE insn, rt, rn\n"
    "  .p2align 4\n"
    "  dmb ish\n"
    "  \\insn \\rt, [\\rn]\n"
    "  ldr \\rn, [sp], #16\n"
    "  br \\rn\n"
    ".endm\n"
    ".macro GOW3_TSO_ALL kind, insn, w, rn\n"
    "  .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30\n"
    "    GOW3_TSO_\\kind \\insn, \\w\\n, \\rn\n"
    "  .endr\n"
    "  GOW3_TSO_\\kind \\insn, \\w\\()zr, \\rn\n"
    ".endm\n"
    ".macro GOW3_TSO_SCRATCH rn\n"
    "  GOW3_TSO_ALL LOAD, ldr, w, \\rn\n"
    "  GOW3_TSO_ALL LOAD, ldr, x, \\rn\n"
    "  GOW3_TSO_ALL LOAD, ldrh, w, \\rn\n"
    "  GOW3_TSO_ALL STORE, str, w, \\rn\n"
    "  GOW3_TSO_ALL STORE, str, x, \\rn\n"
    "  GOW3_TSO_ALL STORE, strh, w, \\rn\n"
    ".endm\n"
    ".p2align 4\n"
    ".globl _gow3_tso_routines\n"
    "_gow3_tso_routines:\n"
    "  GOW3_TSO_SCRATCH x7\n"
    "  GOW3_TSO_SCRATCH x6\n"
    ".globl _gow3_tso_routines_end\n"
    "_gow3_tso_routines_end:\n");
extern const char gow3_tso_routines[], gow3_tso_routines_end[];

// Encodings with Rn and Rt cleared, in routine order; the access size of each.
static const uint32_t k_access_insn[6] = {
    0xB8BFC000u,  // ldapr  wT, [xN]
    0xF8BFC000u,  // ldapr  xT, [xN]
    0x78BFC000u,  // ldaprh wT, [xN]
    0x889FFC00u,  // stlr   wT, [xN]
    0xC89FFC00u,  // stlr   xT, [xN]
    0x489FFC00u,  // stlrh  wT, [xN]
};
static const uint8_t k_access_size[6] = {4, 8, 2, 4, 8, 2};

#define TSO_SLOTS 4096
static _Atomic uint64_t g_tso_total;
static _Atomic uint64_t g_tso_pc[TSO_SLOTS];
static _Atomic uint64_t g_tso_count[TSO_SLOTS];

static void tso_count(uint64_t pc) {
  atomic_fetch_add_explicit(&g_tso_total, 1, memory_order_relaxed);
  uint32_t slot = (uint32_t)(((pc >> 2) * 0x9E3779B97F4A7C15ull) >> 52);
  for (int probe = 0; probe < 32; probe++, slot = (slot + 1) & (TSO_SLOTS - 1)) {
    uint64_t seen = atomic_load_explicit(&g_tso_pc[slot], memory_order_relaxed);
    if (seen == 0 && atomic_compare_exchange_strong(&g_tso_pc[slot], &seen, pc)) seen = pc;
    if (seen == pc) {
      atomic_fetch_add_explicit(&g_tso_count[slot], 1, memory_order_relaxed);
      return;
    }
  }
}

bool tso_crossing_fixup(void *ucontext) {
  struct __darwin_arm_thread_state64 *state = &((ucontext_t *)ucontext)->uc_mcontext->__ss;
  const uint64_t pc = state->__pc;
  if (!recomp_pc_in_guest_code(pc)) return false;
  const uint32_t insn = *(const uint32_t *)(uintptr_t)pc;
  const unsigned rt = insn & 31, rn = (insn >> 5) & 31;
  int access = 0;
  while (access < 6 && (insn & 0xFFFFFC00u) != k_access_insn[access]) access++;
  if (access == 6 || (rn != 7 && rn != 6) || rt == rn) return false;
  const uint64_t address = state->__x[rn];
  if (((address ^ (address + k_access_size[access] - 1)) & 16) == 0) return false;  // not crossing
  tso_count(pc);
  state->__sp -= 16;
  *(uint64_t *)(uintptr_t)state->__sp = pc + 4;
  state->__pc = (uint64_t)(uintptr_t)gow3_tso_routines + ((((rn == 6) * 6 + access) * 32 + rt) * 16);
  return true;
}

// Host address and guest instruction address of every access the peephole rewrote.
struct TsoSite {
  uint64_t host_pc;
  uint64_t guest_address;
};
extern const struct TsoSite g_tso_sites_start[] __asm("section$start$__DATA$__gow3_tso");
extern const struct TsoSite g_tso_sites_end[] __asm("section$end$__DATA$__gow3_tso");

static struct TsoSite *g_tso_sorted;  // the sites by host address, for lookups
static size_t g_tso_sorted_count;

static int tso_site_order(const void *a, const void *b) {
  const uint64_t x = ((const struct TsoSite *)a)->host_pc, y = ((const struct TsoSite *)b)->host_pc;
  return x < y ? -1 : x > y;
}

static uint64_t tso_guest_address(uint64_t pc) {
  if (!g_tso_sorted) {
    g_tso_sorted_count = (size_t)(g_tso_sites_end - g_tso_sites_start);
    g_tso_sorted = malloc(g_tso_sorted_count * sizeof(*g_tso_sorted));
    if (!g_tso_sorted) return 0;
    memcpy(g_tso_sorted, g_tso_sites_start, g_tso_sorted_count * sizeof(*g_tso_sorted));
    qsort(g_tso_sorted, g_tso_sorted_count, sizeof(*g_tso_sorted), tso_site_order);
  }
  size_t lo = 0, hi = g_tso_sorted_count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (g_tso_sorted[mid].host_pc < pc) lo = mid + 1;
    else hi = mid;
  }
  return lo < g_tso_sorted_count && g_tso_sorted[lo].host_pc == pc ? g_tso_sorted[lo].guest_address : 0;
}

// Every 10 s while crossings happen: a log line with the total and the busiest sites, and with
// GOW3_TSO_SITES=<file> all sites as "<guest address> <count>" lines (input for
// tools/tso_crossing_sites.txt).
static void *tso_report_thread(void *arg) {
  (void)arg;
  const char *dump = getenv("GOW3_TSO_SITES");
  uint64_t last = 0;
  for (;;) {
    sleep(10);
    const uint64_t total = atomic_load_explicit(&g_tso_total, memory_order_relaxed);
    if (total == last) continue;
    struct { uint64_t pc, count; } top[12] = {{0, 0}};
    FILE *out = dump && *dump ? fopen(dump, "w") : NULL;
    for (int slot = 0; slot < TSO_SLOTS; slot++) {
      uint64_t pc = atomic_load_explicit(&g_tso_pc[slot], memory_order_relaxed);
      uint64_t count = atomic_load_explicit(&g_tso_count[slot], memory_order_relaxed);
      if (!pc) continue;
      if (out) fprintf(out, "%#llx %llu\n", (unsigned long long)tso_guest_address(pc), (unsigned long long)count);
      if (count <= top[11].count) continue;
      int at = 11;
      while (at > 0 && top[at - 1].count < count) { top[at] = top[at - 1]; at--; }
      top[at].pc = pc;
      top[at].count = count;
    }
    if (out) fclose(out);
    char sites[512];
    int used = 0;
    for (int i = 0; i < 12 && top[i].pc && used < (int)sizeof(sites) - 32; i++)
      used += snprintf(sites + used, sizeof(sites) - (size_t)used, " 0x%llx:%llu",
                       (unsigned long long)tso_guest_address(top[i].pc), (unsigned long long)top[i].count);
    fprintf(stderr, "[tso] 16-byte-crossing accesses: %llu (+%llu in 10 s); top guest sites:%s\n",
            (unsigned long long)total, (unsigned long long)(total - last), used ? sites : " none");
    last = total;
  }
  return NULL;
}

void tso_crossing_start_report(void) {
  if (g_tso_sites_end == g_tso_sites_start) return;  // no rewritten accesses in this build
  pthread_t thread;
  if (pthread_create(&thread, NULL, tso_report_thread, NULL) == 0) pthread_detach(thread);
}
#else
bool tso_crossing_fixup(void *ucontext) {
  (void)ucontext;
  return false;
}
void tso_crossing_start_report(void) {}
#endif
