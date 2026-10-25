#include "ps4_semaphore.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SEMS 1024
#define SEM_MAGIC 0x53454d31 // 'SEM1'

typedef struct {
  uint32_t magic;
  int in_use;
  int value;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} RecompInternalSem;

static RecompInternalSem g_sem_table[MAX_SEMS];
static pthread_mutex_t g_sem_table_lock = PTHREAD_MUTEX_INITIALIZER;

static int alloc_sem_slot(void) {
  pthread_mutex_lock(&g_sem_table_lock);
  for (int i = 1; i < MAX_SEMS; i++) {
    if (!g_sem_table[i].in_use) {
      g_sem_table[i].in_use = 1;
      pthread_mutex_unlock(&g_sem_table_lock);
      return i;
    }
  }
  pthread_mutex_unlock(&g_sem_table_lock);
  return -1;
}

static RecompInternalSem *get_sem(uint32_t id) {
  if (id == 0 || id >= MAX_SEMS) {
    return NULL;
  }
  RecompInternalSem *s = &g_sem_table[id];
  if (!s->in_use || s->magic != SEM_MAGIC) {
    return NULL;
  }
  return s;
}

// sem_init(sem_t *sem, int pshared, unsigned int value)
void shim_sem_init(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  int pshared = (int)ctx->rsi;
  unsigned int value = (unsigned int)ctx->rdx;
  (void)pshared;

  if (!sem_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  int slot = alloc_sem_slot();
  if (slot < 0) {
    set_guest_errno(ctx, ENOSPC);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  RecompInternalSem *s = &g_sem_table[slot];
  s->magic = SEM_MAGIC;
  s->value = (int)value;
  pthread_mutex_init(&s->mutex, NULL);
  pthread_cond_init(&s->cond, NULL);

  // Store slot id in guest sem_t (both 32-bit and 64-bit safe)
  *(uint64_t *)(ctx->mem_base + sem_addr) = (uint64_t)slot;

  ctx->rax = 0;
  SHIM_RETURN();
}

// sem_destroy(sem_t *sem)
void shim_sem_destroy(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  s->magic = 0;
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->mutex);

  pthread_mutex_destroy(&s->mutex);
  pthread_cond_destroy(&s->cond);

  pthread_mutex_lock(&g_sem_table_lock);
  s->in_use = 0;
  pthread_mutex_unlock(&g_sem_table_lock);

  ctx->rax = 0;
  SHIM_RETURN();
}

// sem_wait(sem_t *sem)
void shim_sem_wait(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  while (s->value <= 0) {
    pthread_cond_wait(&s->cond, &s->mutex);
  }
  s->value--;
  pthread_mutex_unlock(&s->mutex);

  ctx->rax = 0;
  SHIM_RETURN();
}

// int sem_timedwait(sem_t *sem, const struct timespec *abs_timeout)
//
// Order of checks as in libkernel (see shadPS4 libraries/kernel/threads/semaphore.cpp,
// posix_sem_timedwait / PthreadSemWait): a NULL or unknown semaphore fails with EINVAL; a
// token that is available is taken before the timeout is looked at, so a bad timespec does not
// fail a wait that would succeed; then tv_nsec outside [0, 1e9) fails with EINVAL; otherwise
// the thread sleeps until the absolute CLOCK_REALTIME deadline and a miss returns -1 with
// errno ETIMEDOUT (FreeBSD value 60, same as the host). A NULL abs_timeout waits without
// limit, like sem_wait. A deadline more than 30 years away is treated the same way, which
// keeps far-future values from overflowing the host's timed wait.
void shim_sem_timedwait(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  uint64_t ts_addr = ctx->rsi;
  RecompInternalSem *s = NULL;
  if (sem_addr) {
    s = get_sem(*(uint32_t *)(ctx->mem_base + sem_addr));
  }
  if (!s) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  if (s->value > 0) {
    s->value--;
    pthread_mutex_unlock(&s->mutex);
    ctx->rax = 0;
    SHIM_RETURN();
  }

  struct timespec deadline = {0, 0};
  int timed = 0;
  if (ts_addr) {
    const int64_t *ts = (const int64_t *)(ctx->mem_base + ts_addr);
    int64_t sec = ts[0], nsec = ts[1];
    if (nsec < 0 || nsec >= 1000000000LL) {
      pthread_mutex_unlock(&s->mutex);
      set_guest_errno(ctx, EINVAL);
      ctx->rax = (uint64_t)-1;
      SHIM_RETURN();
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (sec < (int64_t)now.tv_sec + 30LL * 365 * 86400) {
      deadline.tv_sec = (time_t)sec;
      deadline.tv_nsec = (long)nsec;
      timed = 1;
    }
  }

  while (s->value <= 0 && s->magic == SEM_MAGIC) {
    if (!timed) {
      pthread_cond_wait(&s->cond, &s->mutex);
    } else if (pthread_cond_timedwait(&s->cond, &s->mutex, &deadline) != 0) {
      break; // ETIMEDOUT, or an error that would otherwise spin (EINVAL): report a timeout
    }
  }
  if (s->magic != SEM_MAGIC) { // destroyed while waiting
    pthread_mutex_unlock(&s->mutex);
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }
  if (s->value > 0) { // a post that raced the timeout still wins
    s->value--;
    pthread_mutex_unlock(&s->mutex);
    ctx->rax = 0;
    SHIM_RETURN();
  }
  pthread_mutex_unlock(&s->mutex);
  set_guest_errno(ctx, ETIMEDOUT);
  ctx->rax = (uint64_t)-1;
  SHIM_RETURN();
}

// sem_trywait(sem_t *sem)
void shim_sem_trywait(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  if (s->value > 0) {
    s->value--;
    pthread_mutex_unlock(&s->mutex);
    ctx->rax = 0;
  } else {
    pthread_mutex_unlock(&s->mutex);
    set_guest_errno(ctx, EAGAIN);
    ctx->rax = (uint64_t)-1;
  }
  SHIM_RETURN();
}

// sem_post(sem_t *sem)
void shim_sem_post(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  s->value++;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->mutex);

  ctx->rax = 0;
  SHIM_RETURN();
}

// sem_getvalue(sem_t *sem, int *sval)
void shim_sem_getvalue(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  uint64_t sval_addr = ctx->rsi;
  if (!sem_addr || !sval_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  int cur_val = s->value;
  pthread_mutex_unlock(&s->mutex);

  *(int *)(ctx->mem_base + sval_addr) = cur_val;
  ctx->rax = 0;
  SHIM_RETURN();
}

// scePthread semaphore implementations
void shim_scePthreadSemInit(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  int flag = (int)ctx->rsi;
  unsigned int value = (unsigned int)ctx->rdx;

  if (flag != 0) {
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }
  if (!sem_addr) {
    ctx->rax = 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }

  int slot = alloc_sem_slot();
  if (slot < 0) {
    ctx->rax = 0x8002001c; // ORBIS_KERNEL_ERROR_ENOSPC
    SHIM_RETURN();
  }

  RecompInternalSem *s = &g_sem_table[slot];
  s->magic = SEM_MAGIC;
  s->value = (int)value;
  pthread_mutex_init(&s->mutex, NULL);
  pthread_cond_init(&s->cond, NULL);

  *(uint64_t *)(ctx->mem_base + sem_addr) = (uint64_t)slot;
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSemDestroy(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    ctx->rax = 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  s->magic = 0;
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->mutex);

  pthread_mutex_destroy(&s->mutex);
  pthread_cond_destroy(&s->cond);

  pthread_mutex_lock(&g_sem_table_lock);
  s->in_use = 0;
  pthread_mutex_unlock(&g_sem_table_lock);

  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSemWait(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    ctx->rax = 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  while (s->magic == SEM_MAGIC && s->value <= 0) {
    pthread_cond_wait(&s->cond, &s->mutex);
  }
  if (s->magic != SEM_MAGIC) {
    pthread_mutex_unlock(&s->mutex);
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }
  s->value--;
  pthread_mutex_unlock(&s->mutex);

  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSemTrywait(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    ctx->rax = 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  if (s->value > 0) {
    s->value--;
    pthread_mutex_unlock(&s->mutex);
    ctx->rax = 0;
  } else {
    pthread_mutex_unlock(&s->mutex);
    ctx->rax = 0x80020023; // ORBIS_KERNEL_ERROR_EAGAIN
  }
  SHIM_RETURN();
}

void shim_scePthreadSemTimedwait(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  uint32_t usec = (uint32_t)ctx->rsi;
  if (!sem_addr) {
    ctx->rax = 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  struct timespec ts;
  ts.tv_sec = now.tv_sec + (time_t)(usec / 1000000ULL);
  ts.tv_nsec = now.tv_nsec + (long)((usec % 1000000ULL) * 1000ULL);
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&s->mutex);
  while (s->magic == SEM_MAGIC && s->value <= 0) {
    int rc = pthread_cond_timedwait(&s->cond, &s->mutex, &ts);
    if (rc == ETIMEDOUT) {
      pthread_mutex_unlock(&s->mutex);
      ctx->rax = 0x8002003c; // ORBIS_KERNEL_ERROR_ETIMEDOUT
      SHIM_RETURN();
    }
  }
  if (s->magic != SEM_MAGIC) {
    pthread_mutex_unlock(&s->mutex);
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }
  s->value--;
  pthread_mutex_unlock(&s->mutex);

  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSemPost(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  if (!sem_addr) {
    ctx->rax = 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  s->value++;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->mutex);

  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSemGetvalue(GuestContext *ctx) {
  uint64_t sem_addr = ctx->rdi;
  uint64_t sval_addr = ctx->rsi;
  if (!sem_addr || !sval_addr) {
    ctx->rax = 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }

  uint32_t id = *(uint32_t *)(ctx->mem_base + sem_addr);
  RecompInternalSem *s = get_sem(id);
  if (!s) {
    ctx->rax = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    SHIM_RETURN();
  }

  pthread_mutex_lock(&s->mutex);
  int cur_val = s->value;
  pthread_mutex_unlock(&s->mutex);

  *(int *)(ctx->mem_base + sval_addr) = cur_val;
  ctx->rax = 0;
  SHIM_RETURN();
}

// Orbis Kernel Semaphore implementations
#define MAX_KSEMS 1024
#define KSEM_MAGIC 0x4b53454d // 'KSEM'

typedef struct {
  uint32_t magic;
  int in_use;
  char name[32];
  int32_t current_count;
  int32_t init_count;
  int32_t max_count;
  uint32_t attr;
  int num_waiters;
  int multi_waiters;       // waiters needing more than one unit (they need the broadcast)
  uint64_t last_signal_ns; // GOW3_SEMA_STATS: when the count last rose
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} RecompKernelSema;

// GOW3_SEMA_STATS=1: every 5 s, how many waits blocked and how long a blocked waiter took to
// run after the signal that satisfied it (the wake-up latency the signalling thread waits out
// when it then waits for that work), over all kernel semaphores.
static int g_sema_stats = -1;
static uint64_t g_sema_blocked, g_sema_wake_ns, g_sema_wake_max, g_sema_buckets[4], g_sema_report;
static pthread_mutex_t g_sema_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t sema_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int sema_stats_on(void) {
  if (g_sema_stats < 0) {
    const char *env = getenv("GOW3_SEMA_STATS");
    g_sema_stats = env && env[0] == '1';
  }
  return g_sema_stats;
}

// GOW3_SEMA_SPIN_US (default 40, 0 = off): how long a waiter watches the count before it
// blocks. Waking a blocked thread took 30-120 us on average in gameplay (milliseconds when no core
// was free), ~17 times a frame for God of War III's job workers, while the main thread spun on
// their results. The watch uses ldaxr+wfe: the core sleeps until the count's cache line is
// written (macOS also wakes wfe every ~1 us), so it costs little power.
static int64_t g_sema_spin_ns = -1;
static uint64_t g_sema_spin_hits;

static int64_t sema_spin_ns(void) {
  if (g_sema_spin_ns < 0) {
    const char *env = getenv("GOW3_SEMA_SPIN_US");
    g_sema_spin_ns = (env ? strtoll(env, NULL, 10) : 40) * 1000;
    if (g_sema_spin_ns < 0) g_sema_spin_ns = 0;
  }
  return g_sema_spin_ns;
}

// Watches `count` until it reaches `need` or `ns` pass; returns whether it did.
static int sema_watch(volatile int32_t *count, int32_t need, int64_t ns) {
  const uint64_t deadline = sema_now_ns() + (uint64_t)ns;
  do {
    int32_t v;
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("ldaxr %w0, [%1]" : "=r"(v) : "r"(count) : "memory");
    if (v >= need) {
      __asm__ volatile("clrex" ::: "memory");
      return 1;
    }
    __asm__ volatile("wfe" ::: "memory");
#else
    v = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    if (v >= need) return 1;
#endif
  } while (sema_now_ns() < deadline);
#if defined(__aarch64__) || defined(__arm64__)
  __asm__ volatile("clrex" ::: "memory");
#endif
  return 0;
}

static void sema_note_wake(uint64_t signal_ns) {
  const uint64_t now = sema_now_ns(), ns = signal_ns && now > signal_ns ? now - signal_ns : 0;
  pthread_mutex_lock(&g_sema_stats_lock);
  ++g_sema_blocked;
  g_sema_wake_ns += ns;
  if (ns > g_sema_wake_max) g_sema_wake_max = ns;
  ++g_sema_buckets[ns < 10000 ? 0 : ns < 50000 ? 1 : ns < 200000 ? 2 : 3];
  if (now - g_sema_report >= 5000000000ull) {
    if (g_sema_report) {
      fprintf(stderr, "[sema] %llu waits caught by the watch, %llu blocked in 5 s; wake latency "
                      "mean %.1f us, max %.1f us; <10us %llu, <50us %llu, <200us %llu, longer %llu\n",
              (unsigned long long)g_sema_spin_hits, (unsigned long long)g_sema_blocked,
              g_sema_blocked ? g_sema_wake_ns / 1e3 / g_sema_blocked : 0.0, g_sema_wake_max / 1e3,
              (unsigned long long)g_sema_buckets[0], (unsigned long long)g_sema_buckets[1],
              (unsigned long long)g_sema_buckets[2], (unsigned long long)g_sema_buckets[3]);
    }
    g_sema_blocked = g_sema_wake_ns = g_sema_wake_max = g_sema_spin_hits = 0;
    memset(g_sema_buckets, 0, sizeof(g_sema_buckets));
    g_sema_report = now;
  }
  pthread_mutex_unlock(&g_sema_stats_lock);
}

static RecompKernelSema g_ksem_table[MAX_KSEMS];
static pthread_mutex_t g_ksem_table_lock = PTHREAD_MUTEX_INITIALIZER;

static int alloc_ksem_slot(void) {
  pthread_mutex_lock(&g_ksem_table_lock);
  for (int i = 1; i < MAX_KSEMS; i++) {
    if (!g_ksem_table[i].in_use) {
      g_ksem_table[i].in_use = 1;
      pthread_mutex_unlock(&g_ksem_table_lock);
      return i;
    }
  }
  pthread_mutex_unlock(&g_ksem_table_lock);
  return -1;
}

static RecompKernelSema *get_ksem(OrbisKernelSema id) {
  if (id <= 0 || id >= MAX_KSEMS) return NULL;
  RecompKernelSema *s = &g_ksem_table[id];
  if (!s->in_use || s->magic != KSEM_MAGIC) return NULL;
  return s;
}

int sceKernelCreateSema(OrbisKernelSema *sem, const char *pName, uint32_t attr, int32_t initCount, int32_t maxCount, const void *pOptParam) {
  (void)pOptParam;
  if (!sem || !pName || attr > 2 || initCount < 0 || maxCount <= 0 || initCount > maxCount) {
    return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
  }
  int slot = alloc_ksem_slot();
  if (slot < 0) return 0x80020018; // ORBIS_KERNEL_ERROR_ENFILE

  RecompKernelSema *s = &g_ksem_table[slot];
  s->magic = KSEM_MAGIC;
  strncpy(s->name, pName, sizeof(s->name) - 1);
  s->name[sizeof(s->name) - 1] = '\0';
  s->init_count = initCount;
  s->max_count = maxCount;
  s->current_count = initCount;
  s->attr = attr;
  s->num_waiters = 0;
  s->multi_waiters = 0;
  s->last_signal_ns = 0;
  pthread_mutex_init(&s->mutex, NULL);
  pthread_cond_init(&s->cond, NULL);

  *sem = (OrbisKernelSema)slot;
  return 0;
}

int sceKernelDeleteSema(OrbisKernelSema sem) {
  RecompKernelSema *s = get_ksem(sem);
  if (!s) return 0x80020003; // ORBIS_KERNEL_ERROR_ESRCH

  pthread_mutex_lock(&s->mutex);
  s->magic = 0;
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->mutex);

  pthread_mutex_lock(&g_ksem_table_lock);
  pthread_mutex_destroy(&s->mutex);
  pthread_cond_destroy(&s->cond);
  s->in_use = 0;
  pthread_mutex_unlock(&g_ksem_table_lock);
  return 0;
}

int sceKernelSignalSema(OrbisKernelSema sem, int32_t signalCount) {
  if (signalCount <= 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
  RecompKernelSema *s = get_ksem(sem);
  if (!s) return 0x80020003; // ORBIS_KERNEL_ERROR_ESRCH

  pthread_mutex_lock(&s->mutex);
  if (s->current_count + signalCount > s->max_count) {
    pthread_mutex_unlock(&s->mutex);
    return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
  }
  s->current_count += signalCount;
  if (sema_stats_on()) s->last_signal_ns = sema_now_ns();
  // Wakes as many waiters as units arrived (each needs one), not all of them: God of War III's
  // job workers wait on one semaphore and a broadcast per job woke all five, four to sleep
  // again. A waiter needing more than one unit could miss its turn that way: then all wake.
  if (s->multi_waiters > 0 || signalCount >= s->num_waiters) {
    pthread_cond_broadcast(&s->cond);
  } else {
    for (int32_t i = 0; i < signalCount; i++) {
      pthread_cond_signal(&s->cond);
    }
  }
  pthread_mutex_unlock(&s->mutex);
  return 0;
}

int sceKernelWaitSema(OrbisKernelSema sem, int32_t needCount, uint32_t *pTimeout) {
  if (needCount <= 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
  RecompKernelSema *s = get_ksem(sem);
  if (!s) return 0x80020003; // ORBIS_KERNEL_ERROR_ESRCH

  pthread_mutex_lock(&s->mutex);

  if (s->current_count >= needCount) {
    s->current_count -= needCount;
    pthread_mutex_unlock(&s->mutex);
    return 0;
  }

  if (pTimeout && *pTimeout == 0) {
    pthread_mutex_unlock(&s->mutex);
    return 0x8002003c; // ORBIS_KERNEL_ERROR_ETIMEDOUT
  }

  const int64_t spin = sema_spin_ns();
  if (spin > 0 && !pTimeout) { // a timed wait keeps its exact timeout
    pthread_mutex_unlock(&s->mutex);
    const int caught = sema_watch(&s->current_count, needCount, spin);
    pthread_mutex_lock(&s->mutex);
    if (s->magic != KSEM_MAGIC) {
      pthread_mutex_unlock(&s->mutex);
      return 0x80020003; // ORBIS_KERNEL_ERROR_ESRCH
    }
    if (caught && s->current_count >= needCount) {
      s->current_count -= needCount;
      pthread_mutex_unlock(&s->mutex);
      if (sema_stats_on()) {
        pthread_mutex_lock(&g_sema_stats_lock);
        ++g_sema_spin_hits;
        pthread_mutex_unlock(&g_sema_stats_lock);
      }
      return 0;
    }
  }

  struct timespec ts;
  int has_timeout = 0;
  if (pTimeout) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t usec = *pTimeout;
    ts.tv_sec = now.tv_sec + (time_t)(usec / 1000000ULL);
    ts.tv_nsec = now.tv_nsec + (long)((usec % 1000000ULL) * 1000ULL);
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000L;
    }
    has_timeout = 1;
  }

  s->num_waiters++;
  if (needCount > 1) s->multi_waiters++;
  while (s->magic == KSEM_MAGIC && s->current_count < needCount) {
    if (has_timeout) {
      int rc = pthread_cond_timedwait(&s->cond, &s->mutex, &ts);
      if (rc == ETIMEDOUT) {
        s->num_waiters--;
        if (needCount > 1) s->multi_waiters--;
        if (pTimeout) {
          *pTimeout = 0;
        }
        pthread_mutex_unlock(&s->mutex);
        return 0x8002003c; // ORBIS_KERNEL_ERROR_ETIMEDOUT
      }
    } else {
      pthread_cond_wait(&s->cond, &s->mutex);
    }
  }

  s->num_waiters--;
  if (needCount > 1) s->multi_waiters--;
  if (s->magic != KSEM_MAGIC) {
    pthread_mutex_unlock(&s->mutex);
    return 0x80020003; // ORBIS_KERNEL_ERROR_ESRCH
  }

  s->current_count -= needCount;
  const uint64_t signal_ns = s->last_signal_ns;
  pthread_mutex_unlock(&s->mutex);
  if (sema_stats_on()) sema_note_wake(signal_ns);
  return 0;
}

int sceKernelPollSema(OrbisKernelSema sem, int32_t needCount) {
  if (needCount <= 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
  RecompKernelSema *s = get_ksem(sem);
  if (!s) return 0x80020003; // ORBIS_KERNEL_ERROR_ESRCH

  pthread_mutex_lock(&s->mutex);
  if (s->current_count >= needCount) {
    s->current_count -= needCount;
    pthread_mutex_unlock(&s->mutex);
    return 0;
  }
  pthread_mutex_unlock(&s->mutex);
  return 0x80020010; // ORBIS_KERNEL_ERROR_EBUSY
}

int sceKernelCancelSema(OrbisKernelSema sem, int32_t setCount, int32_t *pNumWaitThreads) {
  RecompKernelSema *s = get_ksem(sem);
  if (!s) return 0x80020003; // ORBIS_KERNEL_ERROR_ESRCH

  pthread_mutex_lock(&s->mutex);
  if (pNumWaitThreads) {
    *pNumWaitThreads = s->num_waiters;
  }
  s->current_count = (setCount < 0) ? s->init_count : setCount;
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->mutex);
  return 0;
}

// Guest shims for Kernel Semaphores
void shim_sceKernelCreateSema(GuestContext *ctx) {
  uint64_t semGuest = ctx->rdi;
  uint64_t pNameGuest = ctx->rsi;
  uint32_t attr = (uint32_t)ctx->rdx;
  int32_t initCount = (int32_t)ctx->rcx;
  int32_t maxCount = (int32_t)ctx->r8;
  uint64_t optGuest = ctx->r9;

  const char *pName = pNameGuest ? (const char *)(ctx->mem_base + pNameGuest) : NULL;
  const void *opt = optGuest ? (const void *)(ctx->mem_base + optGuest) : NULL;

  OrbisKernelSema sem = 0;
  int rc = sceKernelCreateSema(&sem, pName, attr, initCount, maxCount, opt);
  if (rc == 0 && semGuest != 0) {
    *(int32_t *)(ctx->mem_base + semGuest) = sem;
  }
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

void shim_sceKernelDeleteSema(GuestContext *ctx) {
  OrbisKernelSema sem = (OrbisKernelSema)ctx->rdi;
  int rc = sceKernelDeleteSema(sem);
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

void shim_sceKernelWaitSema(GuestContext *ctx) {
  OrbisKernelSema sem = (OrbisKernelSema)ctx->rdi;
  int32_t needCount = (int32_t)ctx->rsi;
  uint64_t timeoutGuest = ctx->rdx;

  uint32_t *pTimeout = timeoutGuest ? (uint32_t *)(ctx->mem_base + timeoutGuest) : NULL;
  int rc = sceKernelWaitSema(sem, needCount, pTimeout);
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

void shim_sceKernelSignalSema(GuestContext *ctx) {
  OrbisKernelSema sem = (OrbisKernelSema)ctx->rdi;
  int32_t signalCount = (int32_t)ctx->rsi;
  int rc = sceKernelSignalSema(sem, signalCount);
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

void shim_sceKernelPollSema(GuestContext *ctx) {
  OrbisKernelSema sem = (OrbisKernelSema)ctx->rdi;
  int32_t needCount = (int32_t)ctx->rsi;
  int rc = sceKernelPollSema(sem, needCount);
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

void shim_sceKernelCancelSema(GuestContext *ctx) {
  OrbisKernelSema sem = (OrbisKernelSema)ctx->rdi;
  int32_t setCount = (int32_t)ctx->rsi;
  uint64_t numWaitGuest = ctx->rdx;

  int32_t *pNumWait = numWaitGuest ? (int32_t *)(ctx->mem_base + numWaitGuest) : NULL;
  int rc = sceKernelCancelSema(sem, setCount, pNumWait);
  ctx->rax = (uint64_t)(int64_t)rc;
  SHIM_RETURN();
}

