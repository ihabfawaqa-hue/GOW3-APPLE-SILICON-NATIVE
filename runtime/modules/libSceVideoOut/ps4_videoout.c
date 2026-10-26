#include "ps4_videoout.h"
#include "ps4_metal_screen.h"
#include "../../core/gow3gpu_gnm_adapter.h"
#include "../../core/gow3gpu_runtime_loader.h"
#include "../../core/recomp_context.h"
#include "../libkernel/ps4_kernel_sys.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>

__attribute__((weak)) int ps4_gnm_metal_present(GuestContext *ctx, uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch,
                          uint32_t tiling_mode, uint32_t pixel_format, void (*on_done)(void *), void *arg) {
    (void)ctx; (void)addr; (void)width; (void)height; (void)pitch; (void)tiling_mode; (void)pixel_format; (void)on_done; (void)arg;
    return 0;
}
__attribute__((weak)) int ps4_gnm_read_display_cpu(GuestContext *ctx, uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t tiling_mode, uint32_t pixel_format, uint8_t *out_bgra) {
    (void)ctx; (void)addr; (void)width; (void)height; (void)pitch; (void)tiling_mode; (void)pixel_format; (void)out_bgra;
    return 0;
}

typedef struct Gow3GpuVideoOutBufferAttribute {
    uint32_t pixel_format;
    int32_t tiling_mode;
    int32_t aspect_ratio;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_in_pixel;
    uint32_t option;
    uint32_t reserved0;
    uint64_t reserved1;
} Gow3GpuVideoOutBufferAttribute;

typedef int32_t (*Gow3GpuVideoOutOpenFn)(int32_t, int32_t, int32_t, const void *);
typedef int32_t (*Gow3GpuVideoOutCloseFn)(int32_t);
typedef int32_t (*Gow3GpuVideoOutRegisterBuffersFn)(int32_t, int32_t,
                                                     void *const *, int32_t,
                                                     const Gow3GpuVideoOutBufferAttribute *);
typedef int32_t (*Gow3GpuVideoOutUnregisterBuffersFn)(int32_t, int32_t);
typedef int32_t (*Gow3GpuVideoOutSetFlipRateFn)(int32_t, int32_t);
typedef int32_t (*Gow3GpuVideoOutSubmitFlipFn)(int32_t, int32_t, int32_t, int64_t);

static int gow3gpu_videoout_active(void) {
    return ps4_gow3gpu_submission_backend_active();
}

static uintptr_t gow3gpu_videoout_symbol(const char *nid) {
    char scoped_nid[96];
    const int length = snprintf(scoped_nid, sizeof(scoped_nid),
                                "%s#libSceVideoOut#libSceVideoOut", nid);
    if (length < 0 || (size_t)length >= sizeof(scoped_nid)) return 0;
    return ps4_gow3gpu_resolve_candidate(scoped_nid);
}
#include <sys/time.h>

#define MAX_VIDEO_HANDLES 8
#define MAX_FRAME_BUFFERS 16
#define MAX_EQUEUES_PER_PORT 16

typedef struct {
    int in_use;
    int32_t handle;
    OrbisVideoOutBufferAttribute attr;
    int bufferCount;
    uint64_t buffers[MAX_FRAME_BUFFERS];
    OrbisKernelEqueue flipQueues[MAX_EQUEUES_PER_PORT];
    void *flipQueueUdatas[MAX_EQUEUES_PER_PORT];
    int flipQueueCount;
    int32_t flipRate;
    OrbisVideoOutFlipStatus flipStatus;
    pthread_mutex_t mutex;

    pthread_cond_t vblankCond;
    SceVideoOutVblankStatus vblankStatus;
    OrbisKernelEqueue vblankQueues[MAX_EQUEUES_PER_PORT];
    void *vblankUdatas[MAX_EQUEUES_PER_PORT];
    int vblankQueueCount;
} VideoOutHandle;

static VideoOutHandle g_video_handles[MAX_VIDEO_HANDLES];
static pthread_mutex_t g_video_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_next_video_handle = 1;

static pthread_t g_vblank_thread;
static int g_vblank_thread_started = 0;
static volatile int g_vblank_thread_running = 1;

// VBlank and flip status times use the guest's clocks: process time in microseconds
// (sceKernelGetProcessTime) and the TSC at RECOMP_TSC_FREQ (sceKernelReadTsc). They were the
// Unix time and the raw 24 MHz counter, so a game comparing them with its own clock readings
// (frame pacing, movie and audio sync) saw vblanks advance 88.75 times too slowly.
static uint64_t get_time_usec(void) {
    return sceKernelGetProcessTime();
}

static uint64_t get_fake_tsc(void) {
    return recomp_rdtsc();
}

static void *vblank_thread_func(void *arg) {
    (void)arg;
    // Absolute deadlines: a relative sleep adds each tick's own work and wake latency to the period
    // and drifts 1-3% slow against CoreAudio's wall-clock 48 kHz. Slot N is start + N * 1/60 s.
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint64_t period_abs = (uint64_t)(1000000000.0 / 60.0) * tb.denom / tb.numer;
    uint64_t next_abs = mach_absolute_time() + period_abs;
    const int vblank_log = getenv("PS4_VBLANK_LOG") != NULL; // wall time per 600 ticks: expect 10.000 s
    const uint64_t log_start_abs = mach_absolute_time();
    uint64_t ticks = 0;

    while (g_vblank_thread_running) {
        mach_wait_until(next_abs);
        next_abs += period_abs;
        if (vblank_log && ++ticks % 600 == 0)
            fprintf(stderr, "[vblank] %llu ticks in %.3f s\n", (unsigned long long)ticks,
                    (double)(mach_absolute_time() - log_start_abs) * tb.numer / tb.denom / 1e9);
        const uint64_t now_abs = mach_absolute_time();
        if (now_abs > next_abs + 4 * period_abs) next_abs = now_abs + period_abs; // suspended: resync

        pthread_mutex_lock(&g_video_table_mutex);
        for (int i = 0; i < MAX_VIDEO_HANDLES; i++) {
            VideoOutHandle *vh = &g_video_handles[i];
            if (!vh->in_use) continue;

            pthread_mutex_lock(&vh->mutex);
            vh->vblankStatus.count++;
            vh->vblankStatus.process_time = get_time_usec();
            vh->vblankStatus.tsc = get_fake_tsc();

            pthread_cond_broadcast(&vh->vblankCond);

            uint64_t vblank_time_12b = vh->vblankStatus.tsc & 0xfffULL;
            uint64_t vblank_counter_4b = (uint64_t)(vh->vblankStatus.count & 0xfULL);
            uint64_t vblank_event_data = vblank_time_12b | (vblank_counter_4b << 12) | ((uint64_t)vh->vblankStatus.count << 16);

            for (int q = 0; q < vh->vblankQueueCount; q++) {
                if (vh->vblankQueues[q] > 0) {
                    ps4_equeue_trigger_event(vh->vblankQueues[q], 0x7 /* Vblank */, ORBIS_KERNEL_EVFILT_VIDEO_OUT, (int64_t)vblank_event_data, vh->vblankUdatas[q]);
                }
            }
            pthread_mutex_unlock(&vh->mutex);
        }
        pthread_mutex_unlock(&g_video_table_mutex);
    }
    return NULL;
}

static VideoOutHandle *find_video_handle_locked(int32_t handle) {
    if (handle <= 0) return NULL;
    for (int i = 0; i < MAX_VIDEO_HANDLES; i++) {
        if (g_video_handles[i].in_use && g_video_handles[i].handle == handle) {
            return &g_video_handles[i];
        }
    }
    return NULL;
}

int32_t sceVideoOutOpen(int32_t userId, int32_t busType, int32_t index, const void *param) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = NULL;
    for (int i = 0; i < MAX_VIDEO_HANDLES; i++) {
        if (!g_video_handles[i].in_use) {
            vh = &g_video_handles[i];
            break;
        }
    }

    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -ENOMEM;
    }

    vh->handle = g_next_video_handle++;
    vh->in_use = 1;
    vh->bufferCount = 0;
    vh->flipQueueCount = 0;
    memset(vh->flipQueues, 0, sizeof(vh->flipQueues));
    memset(vh->flipQueueUdatas, 0, sizeof(vh->flipQueueUdatas));
    vh->flipRate = 0;
    vh->vblankQueueCount = 0;
    memset(&vh->attr, 0, sizeof(vh->attr));
    memset(&vh->flipStatus, 0, sizeof(vh->flipStatus));
    memset(&vh->vblankStatus, 0, sizeof(vh->vblankStatus));
    memset(vh->vblankQueues, 0, sizeof(vh->vblankQueues));
    memset(vh->vblankUdatas, 0, sizeof(vh->vblankUdatas));
    pthread_mutex_init(&vh->mutex, NULL);
    pthread_cond_init(&vh->vblankCond, NULL);

    if (!g_vblank_thread_started) {
        g_vblank_thread_running = 1;
        pthread_create(&g_vblank_thread, NULL, vblank_thread_func, NULL);
        g_vblank_thread_started = 1;
    }

    int32_t ret = vh->handle;
    pthread_mutex_unlock(&g_video_table_mutex);

    if (gow3gpu_videoout_active()) {
        Gow3GpuVideoOutOpenFn candidate_open =
            (Gow3GpuVideoOutOpenFn)gow3gpu_videoout_symbol("Up36PTk687E");
        const int32_t candidate_handle = candidate_open
            ? candidate_open(userId, busType, index, param)
            : -ENOSYS;
        if (candidate_handle != ret) {
            fprintf(stderr,
                    "[gow3gpu-route] VideoOut open mismatch: primary=%d candidate=%d\n",
                    ret, candidate_handle);
            (void)sceVideoOutClose(ret);
            return candidate_handle < 0 ? candidate_handle : -EIO;
        }
        fprintf(stderr,
                "[gow3gpu-route] VideoOut handle %d opened in shadPS4 Vulkan presenter\n",
                ret);
    } else {
        // The default path remains the native Metal VideoOut implementation.
        ps4_metal_screen_init(1920, 1080, "PS4Recomp - Native Metal Screen");
    }

    return ret;
}

int32_t sceVideoOutClose(int32_t handle) {
    if (gow3gpu_videoout_active()) {
        Gow3GpuVideoOutCloseFn candidate_close =
            (Gow3GpuVideoOutCloseFn)gow3gpu_videoout_symbol("uquVH4-Du78");
        if (candidate_close) (void)candidate_close(handle);
    }
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    vh->in_use = 0;
    pthread_cond_broadcast(&vh->vblankCond);
    pthread_mutex_unlock(&vh->mutex);
    pthread_mutex_destroy(&vh->mutex);
    pthread_cond_destroy(&vh->vblankCond);

    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

void ps4_videoout_destroy(void) {
    if (g_vblank_thread_started) {
        g_vblank_thread_running = 0;
        pthread_join(g_vblank_thread, NULL);
        g_vblank_thread_started = 0;
    }
    pthread_mutex_lock(&g_video_table_mutex);
    for (int i = 0; i < MAX_VIDEO_HANDLES; i++) {
        if (g_video_handles[i].in_use) {
            pthread_mutex_destroy(&g_video_handles[i].mutex);
            pthread_cond_destroy(&g_video_handles[i].vblankCond);
            g_video_handles[i].in_use = 0;
        }
    }
    pthread_mutex_unlock(&g_video_table_mutex);
}

void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *attr, uint32_t pixelFormat, uint32_t tilingMode, uint32_t aspectRatio, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!attr) return;
    attr->format = (int32_t)pixelFormat;
    attr->tmode = (int32_t)tilingMode;
    attr->aspect = (int32_t)aspectRatio;
    attr->width = width;
    attr->height = height;
    attr->pixelPitch = pitch;
    attr->reserved[0] = 0;
    attr->reserved[1] = 0;
}

int32_t sceVideoOutRegisterBuffers(int32_t handle, int32_t startIndex, void * const *bufferArray, int32_t bufferCount, const OrbisVideoOutBufferAttribute *attr) {
    if (gow3gpu_videoout_active() &&
        (!bufferArray || !attr || startIndex < 0 || bufferCount < 0 ||
         startIndex > MAX_FRAME_BUFFERS ||
         bufferCount > MAX_FRAME_BUFFERS - startIndex)) {
        fprintf(stderr, "[gow3gpu-route] invalid VideoOut buffer registration\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);

    if (attr) {
        vh->attr = *attr;
    }

    if (bufferArray && bufferCount > 0) {
        for (int i = 0; i < bufferCount && (startIndex + i) < MAX_FRAME_BUFFERS; i++) {
            vh->buffers[startIndex + i] = (uint64_t)bufferArray[i];
        }
        if (startIndex + bufferCount > vh->bufferCount) {
            vh->bufferCount = startIndex + bufferCount;
        }
    }

    pthread_mutex_unlock(&vh->mutex);
    if (!gow3gpu_videoout_active()) return 0;
    Gow3GpuVideoOutRegisterBuffersFn candidate_register =
        (Gow3GpuVideoOutRegisterBuffersFn)gow3gpu_videoout_symbol("w3BY+tAEiQY");
    if (!candidate_register) return -ENOSYS;
    const Gow3GpuVideoOutBufferAttribute candidate_attr = {
        .pixel_format = (uint32_t)attr->format,
        .tiling_mode = attr->tmode,
        .aspect_ratio = attr->aspect,
        .width = attr->width,
        .height = attr->height,
        .pitch_in_pixel = attr->pixelPitch,
        .option = 0,
        .reserved0 = 0,
        .reserved1 = 0,
    };
    const int32_t candidate_result =
        candidate_register(handle, startIndex, bufferArray, bufferCount,
                           &candidate_attr);
    if (candidate_result < 0) {
        fprintf(stderr,
                "[gow3gpu-route] candidate VideoOut buffer registration failed: 0x%x\n",
                (unsigned)candidate_result);
        return candidate_result;
    }
    fprintf(stderr,
            "[gow3gpu-route] registered %d display buffers in shadPS4 Vulkan VideoOut\n",
            bufferCount);
    return candidate_result;
}

int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t fliprate) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    if (vh->flipRate != fliprate) fprintf(stderr, "[videoout] SetFlipRate(handle=%d, rate=%d)\n", handle, fliprate);
    vh->flipRate = fliprate;
    pthread_mutex_unlock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);

    if (gow3gpu_videoout_active()) {
        Gow3GpuVideoOutSetFlipRateFn candidate_set_rate =
            (Gow3GpuVideoOutSetFlipRateFn)gow3gpu_videoout_symbol("CBiu4mCE1DA");
        return candidate_set_rate ? candidate_set_rate(handle, fliprate) : -ENOSYS;
    } else {
        ps4_metal_screen_set_guest_flip_rate(fliprate);
    }
    return 0;
}

int32_t sceVideoOutAddFlipEvent(OrbisKernelEqueue eq, int32_t handle, void *flipArg) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    int found = 0;
    for (int i = 0; i < vh->flipQueueCount; i++) {
        if (vh->flipQueues[i] == eq) {
            vh->flipQueueUdatas[i] = flipArg;
            found = 1;
            break;
        }
    }
    if (!found && vh->flipQueueCount < MAX_EQUEUES_PER_PORT) {
        vh->flipQueues[vh->flipQueueCount] = eq;
        vh->flipQueueUdatas[vh->flipQueueCount] = flipArg;
        vh->flipQueueCount++;
    }
    pthread_mutex_unlock(&vh->mutex);

    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

// Number of flips submitted so far (debug clock for PS4_PAD_CLOCK=frames).
volatile uint64_t g_ps4_videoout_flip_count;

// A flip completes when the GPU has executed the frame's presentation (like the display engine
// flipping after the GPU wrote the buffer): flip status and the flip event are updated then.
typedef struct PendingFlip {
    int32_t handle;
    int32_t bufferIndex;
    int64_t flipArg;
} PendingFlip;

static void finish_flip(const PendingFlip *pf) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(pf->handle);
    if (vh) {
        pthread_mutex_lock(&vh->mutex);
        vh->flipStatus.num++;
        vh->flipStatus.flipArg = pf->flipArg;
        vh->flipStatus.currentBuffer = pf->bufferIndex;
        if (vh->flipStatus.numFlipPending > 0) vh->flipStatus.numFlipPending--;
        if (vh->flipStatus.numGpuFlipPending > 0) vh->flipStatus.numGpuFlipPending--;
        vh->flipStatus.ptime = get_time_usec();
        vh->flipStatus.stime = get_fake_tsc();
        // Event data: low 12 bits of the timestamp, 4-bit flip counter, flip argument above.
        uint64_t time_12b = vh->flipStatus.stime & 0xfffULL;
        uint64_t counter_4b = (uint64_t)(vh->flipStatus.num & 0xfULL);
        uint64_t event_data = time_12b | (counter_4b << 12) | ((uint64_t)pf->flipArg << 16);
        for (int q = 0; q < vh->flipQueueCount; q++) {
            if (vh->flipQueues[q] > 0) {
                ps4_equeue_trigger_event(vh->flipQueues[q], 0x6 /* Flip */, ORBIS_KERNEL_EVFILT_VIDEO_OUT,
                                         (int64_t)event_data, vh->flipQueueUdatas[q]);
            }
        }
        pthread_mutex_unlock(&vh->mutex);
    }
    pthread_mutex_unlock(&g_video_table_mutex);
}

static void complete_flip(void *arg) {
    finish_flip((const PendingFlip *)arg);
    free(arg);
}

void ps4_videoout_complete_external_flip(int32_t handle, int32_t bufferIndex,
                                        int64_t flipArg) {
    const PendingFlip completed = {handle, bufferIndex, flipArg};
    finish_flip(&completed);
    static uint32_t candidate_completions;
    if ((candidate_completions++ % 120) == 0) {
        fprintf(stderr,
                "[gow3gpu-route] candidate Vulkan VideoOut presenter handoff %u (buffer=%d)\n",
                candidate_completions, bufferIndex);
    }
}

static void rollback_flip_pending(int32_t handle) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (vh) {
        pthread_mutex_lock(&vh->mutex);
        if (vh->flipStatus.numFlipPending > 0) vh->flipStatus.numFlipPending--;
        if (vh->flipStatus.numGpuFlipPending > 0) vh->flipStatus.numGpuFlipPending--;
        pthread_mutex_unlock(&vh->mutex);
    }
    pthread_mutex_unlock(&g_video_table_mutex);
}

int32_t sceVideoOutSubmitFlip(GuestContext *ctx, int32_t handle, int32_t bufferIndex, uint32_t flipMode, int64_t flipArg) {
    static uint32_t s_last_mode = ~0u;
    if (__atomic_exchange_n(&s_last_mode, flipMode, __ATOMIC_RELAXED) != flipMode)
        fprintf(stderr, "[videoout] SubmitFlip flip mode %u\n", flipMode);
    if (!gow3gpu_videoout_active()) ps4_metal_screen_set_guest_flip_mode((int)flipMode);
    __atomic_fetch_add(&g_ps4_videoout_flip_count, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }
    pthread_mutex_lock(&vh->mutex);
    // bufferIndex -1 is a flip without a display buffer (blank frame): it only completes the flip.
    if (bufferIndex != -1 && (bufferIndex < 0 || bufferIndex >= vh->bufferCount)) {
        pthread_mutex_unlock(&vh->mutex);
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }
    const uint64_t guest_buf_vaddr = bufferIndex >= 0 ? vh->buffers[bufferIndex] : 0;
    const OrbisVideoOutBufferAttribute attr = vh->attr;
    vh->flipStatus.numFlipPending++;
    vh->flipStatus.numGpuFlipPending++;
    pthread_mutex_unlock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);

    if (gow3gpu_videoout_active()) {
        Gow3GpuVideoOutSubmitFlipFn candidate_submit_flip =
            (Gow3GpuVideoOutSubmitFlipFn)gow3gpu_videoout_symbol("U46NwOiJpys");
        if (!candidate_submit_flip) {
            rollback_flip_pending(handle);
            return -ENOSYS;
        }
        const int32_t candidate_result =
            candidate_submit_flip(handle, bufferIndex, (int32_t)flipMode, flipArg);
        if (candidate_result != 0) {
            rollback_flip_pending(handle);
            fprintf(stderr,
                    "[gow3gpu-route] candidate VideoOut flip failed: 0x%x\n",
                    (unsigned)candidate_result);
            return candidate_result;
        }
        static uint32_t candidate_flips;
        if ((candidate_flips++ % 120) == 0) {
            fprintf(stderr,
                    "[gow3gpu-route] candidate Vulkan VideoOut accepted flip %u (buffer=%d)\n",
                    candidate_flips, bufferIndex);
        }
        ps4_stall_watchdog_note_present();
        return 0;
    }

    PendingFlip *pf = (PendingFlip *)malloc(sizeof(PendingFlip));
    if (pf) {
        pf->handle = handle;
        pf->bufferIndex = bufferIndex;
        pf->flipArg = flipArg;
    }
    // No VideoOut lock is held while presenting: the completion may run on the GPU completion
    // thread at any time and takes the locks itself.
    int queued = 0;
    if (ctx && guest_buf_vaddr != 0) {
        int width = attr.width > 0 ? (int)attr.width : 1920;
        int height = attr.height > 0 ? (int)attr.height : 1080;
        uint32_t pitch_px = attr.pixelPitch > 0 ? attr.pixelPitch : (uint32_t)width;
        static uint64_t s_presents;
        ps4_stall_watchdog_note_present();
        if ((s_presents++ % 300) == 0)
            fprintf(stderr, "[videoout] Presenting frame %lld (buffer %d, %dx%d, tmode %d, fmt 0x%x)\n", (long long)flipArg,
                    bufferIndex, width, height, attr.tmode, (unsigned)attr.format);
        // GPU-rendered display buffer: present straight from the Metal render target.
        if (pf && ps4_gnm_metal_present(ctx, guest_buf_vaddr, (uint32_t)width, (uint32_t)height, pitch_px,
                                        (uint32_t)attr.tmode, (uint32_t)attr.format, complete_flip, pf)) {
            queued = 1;
        } else {
            // Otherwise detile the guest memory on the CPU.
            static uint8_t *s_buf;
            static size_t s_buf_size;
            size_t need = (size_t)width * height * 4;
            if (s_buf_size < need) {
                free(s_buf);
                s_buf = (uint8_t *)malloc(need);
                s_buf_size = s_buf ? need : 0;
            }
            if (s_buf && ps4_gnm_read_display_cpu(ctx, guest_buf_vaddr, (uint32_t)width, (uint32_t)height, pitch_px,
                                                  (uint32_t)attr.tmode, (uint32_t)attr.format, s_buf)) {
                ps4_metal_screen_present_frame(s_buf, (size_t)width * 4, width, height);
            } else {
                ps4_metal_screen_present_frame(ctx->mem_base + guest_buf_vaddr, (size_t)pitch_px * 4, width, height);
            }
        }
    }
    if (!queued) {
        const PendingFlip now = {handle, bufferIndex, flipArg};
        finish_flip(&now);
        free(pf);
    }
    return 0;
}

int32_t sceVideoOutGetFlipStatus(int32_t handle, OrbisVideoOutFlipStatus *status) {
    if (!status) return -EINVAL;

    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    *status = vh->flipStatus;
    // Polled every frame: logged on the first call only.
    static int s_logged;
    if (!__atomic_exchange_n(&s_logged, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "[videoout] sceVideoOutGetFlipStatus: handle=%d num=%llu currentBuf=%d (first call)\n",
                handle, (unsigned long long)vh->flipStatus.num, vh->flipStatus.currentBuffer);
    pthread_mutex_unlock(&vh->mutex);

    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

// Shims for guest execution
void shim_sceVideoOutOpen(GuestContext *ctx) {
    int32_t userId = (int32_t)ctx->rdi;
    int32_t busType = (int32_t)ctx->rsi;
    int32_t index = (int32_t)ctx->rdx;
    uint64_t paramGuest = ctx->rcx;
    const void *param = paramGuest ? (const void *)(ctx->mem_base + paramGuest) : NULL;

    int32_t h = sceVideoOutOpen(userId, busType, index, param);
    ctx->rax = (uint64_t)(int64_t)h;
    SHIM_RETURN();
}

void shim_sceVideoOutClose(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    ctx->rax = (uint64_t)(int64_t)sceVideoOutClose(handle);
    SHIM_RETURN();
}

void shim_sceVideoOutSetBufferAttribute(GuestContext *ctx) {
    uint64_t attrGuest = ctx->rdi;
    uint32_t pixelFormat = (uint32_t)ctx->rsi;
    uint32_t tilingMode = (uint32_t)ctx->rdx;
    uint32_t aspectRatio = (uint32_t)ctx->rcx;
    uint32_t width = (uint32_t)ctx->r8;
    uint32_t height = (uint32_t)ctx->r9;
    // 7th argument passed on stack in System V AMD64 ABI: [rsp + 8]
    uint64_t rsp = ctx->rsp;
    uint32_t pitch = (uint32_t)*(uint64_t *)(ctx->mem_base + rsp + 8);

    OrbisVideoOutBufferAttribute *attr = attrGuest ? (OrbisVideoOutBufferAttribute *)(ctx->mem_base + attrGuest) : NULL;
    sceVideoOutSetBufferAttribute(attr, pixelFormat, tilingMode, aspectRatio, width, height, pitch);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceVideoOutRegisterBuffers(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    int32_t startIndex = (int32_t)ctx->rsi;
    uint64_t bufferArrayGuest = ctx->rdx;
    int32_t bufferCount = (int32_t)ctx->rcx;
    uint64_t attrGuest = ctx->r8;

    void * const *bufferArray = bufferArrayGuest ? (void * const *)(ctx->mem_base + bufferArrayGuest) : NULL;
    const OrbisVideoOutBufferAttribute *attr = attrGuest ? (const OrbisVideoOutBufferAttribute *)(ctx->mem_base + attrGuest) : NULL;

    int32_t rc = sceVideoOutRegisterBuffers(handle, startIndex, bufferArray, bufferCount, attr);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutSetFlipRate(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    int32_t fliprate = (int32_t)ctx->rsi;
    ctx->rax = (uint64_t)(int64_t)sceVideoOutSetFlipRate(handle, fliprate);
    SHIM_RETURN();
}

void shim_sceVideoOutAddFlipEvent(GuestContext *ctx) {
    OrbisKernelEqueue eq = (OrbisKernelEqueue)ctx->rdi;
    int32_t handle = (int32_t)ctx->rsi;
    // udata is returned to the guest verbatim, so keep the guest value.
    void *flipArg = (void *)(uintptr_t)ctx->rdx;

    ctx->rax = (uint64_t)(int64_t)sceVideoOutAddFlipEvent(eq, handle, flipArg);
    fprintf(stderr, "[videoout] AddFlipEvent(eq=%lld, handle=%d, udata=%p) -> %d\n", (long long)(intptr_t)eq, handle, flipArg, (int)ctx->rax);
    SHIM_RETURN();
}

void shim_sceVideoOutSubmitFlip(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    int32_t bufferIndex = (int32_t)ctx->rsi;
    uint32_t flipMode = (uint32_t)ctx->rdx;
    int64_t flipArg = (int64_t)ctx->rcx;

    int32_t rc = sceVideoOutSubmitFlip(ctx, handle, bufferIndex, flipMode, flipArg);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutGetFlipStatus(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    uint64_t statusGuest = ctx->rsi;
    OrbisVideoOutFlipStatus *status = statusGuest ? (OrbisVideoOutFlipStatus *)(ctx->mem_base + statusGuest) : NULL;

    ctx->rax = (uint64_t)(int64_t)sceVideoOutGetFlipStatus(handle, status);
    SHIM_RETURN();
}

int32_t sceVideoOutGetResolutionStatus(int32_t handle, OrbisVideoOutResolutionStatus *status) {
    (void)handle;
    if (!status) {
        return -EINVAL;
    }
    memset(status, 0, sizeof(*status));
    status->width = 1920;
    status->height = 1080;
    status->paneWidth = 1920;
    status->paneHeight = 1080;
    status->refreshRate = 60;
    status->screenSize = 55.0f;
    return 0;
}

int32_t sceVideoOutIsFlipPending(int32_t handle) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return 0;
    }
    pthread_mutex_lock(&vh->mutex);
    int32_t pending = vh->flipStatus.numFlipPending;
    pthread_mutex_unlock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);
    return pending;
}

int32_t sceVideoOutUnregisterBuffers(int32_t handle, int32_t setIndex) {
    if (gow3gpu_videoout_active()) {
        Gow3GpuVideoOutUnregisterBuffersFn candidate_unregister =
            (Gow3GpuVideoOutUnregisterBuffersFn)gow3gpu_videoout_symbol("N5KDtkIjjJ4");
        if (!candidate_unregister) return -ENOSYS;
        return candidate_unregister(handle, setIndex);
    }
    (void)handle;
    (void)setIndex;
    return 0;
}

void shim_sceVideoOutGetResolutionStatus(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    uint64_t statusGuest = ctx->rsi;
    OrbisVideoOutResolutionStatus *status = statusGuest ? (OrbisVideoOutResolutionStatus *)(ctx->mem_base + statusGuest) : NULL;

    ctx->rax = (uint64_t)(int64_t)sceVideoOutGetResolutionStatus(handle, status);
    SHIM_RETURN();
}

void shim_sceVideoOutIsFlipPending(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    ctx->rax = (uint64_t)(int64_t)sceVideoOutIsFlipPending(handle);
    SHIM_RETURN();
}

void shim_sceVideoOutUnregisterBuffers(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    int32_t setIndex = (int32_t)ctx->rsi;
    ctx->rax = (uint64_t)(int64_t)sceVideoOutUnregisterBuffers(handle, setIndex);
    SHIM_RETURN();
}

int32_t sceVideoOutDeleteFlipEvent(OrbisKernelEqueue eq, int32_t handle) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    for (int i = 0; i < vh->flipQueueCount; i++) {
        if (vh->flipQueues[i] == eq) {
            for (int j = i; j < vh->flipQueueCount - 1; j++) {
                vh->flipQueues[j] = vh->flipQueues[j + 1];
                vh->flipQueueUdatas[j] = vh->flipQueueUdatas[j + 1];
            }
            vh->flipQueueCount--;
            break;
        }
    }
    pthread_mutex_unlock(&vh->mutex);

    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

int32_t sceVideoOutGetEventData(const OrbisKernelEvent *ev, int64_t *data) {
    if (!ev || !data) {
        return -EINVAL;
    }
    if (ev->filter != ORBIS_KERNEL_EVFILT_VIDEO_OUT) {
        return -EINVAL;
    }

    int64_t event_data = ev->data >> 16;
    if (ev->ident != 0x6 || ev->data >= 0) {
        *data = event_data;
    } else {
        *data = event_data | (int64_t)0xffff000000000000ULL;
    }
    return 0;
}

int32_t sceVideoOutConfigureOutputMode_(int32_t handle, uint32_t reserved, const void *mode, const void *options, uint32_t size_mode, uint32_t size_options) {
    (void)options;
    (void)size_mode;
    (void)size_options;
    (void)mode;

    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    if (reserved != 0) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

void sceVideoOutConfigureOptionsInitialize_(void *options, uint32_t size) {
    if (options && size >= sizeof(uint32_t)) {
        memset(options, 0, size);
        *(uint32_t *)options = size;
    }
}

void sceVideoOutModeSetAny_(void *mode, uint32_t size) {
    if (mode && size >= sizeof(uint32_t)) {
        memset(mode, 0xff, size);
        *(uint32_t *)mode = size;
    }
}

int32_t sceVideoOutAddVblankEvent(OrbisKernelEqueue eq, int32_t handle, void *udata) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    for (int i = 0; i < vh->vblankQueueCount; i++) {
        if (vh->vblankQueues[i] == eq) {
            vh->vblankUdatas[i] = udata;
            pthread_mutex_unlock(&vh->mutex);
            pthread_mutex_unlock(&g_video_table_mutex);
            return 0;
        }
    }
    if (vh->vblankQueueCount < MAX_EQUEUES_PER_PORT) {
        vh->vblankQueues[vh->vblankQueueCount] = eq;
        vh->vblankUdatas[vh->vblankQueueCount] = udata;
        vh->vblankQueueCount++;
    }
    pthread_mutex_unlock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

int32_t sceVideoOutDeleteVblankEvent(OrbisKernelEqueue eq, int32_t handle) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    for (int i = 0; i < vh->vblankQueueCount; i++) {
        if (vh->vblankQueues[i] == eq) {
            for (int j = i; j < vh->vblankQueueCount - 1; j++) {
                vh->vblankQueues[j] = vh->vblankQueues[j + 1];
                vh->vblankUdatas[j] = vh->vblankUdatas[j + 1];
            }
            vh->vblankQueueCount--;
            break;
        }
    }
    pthread_mutex_unlock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

int32_t sceVideoOutWaitVblank(int32_t handle) {
    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);

    uint64_t cur_count = vh->vblankStatus.count;
    while (vh->in_use && vh->vblankStatus.count == cur_count) {
        pthread_cond_wait(&vh->vblankCond, &vh->mutex);
    }
    pthread_mutex_unlock(&vh->mutex);
    return 0;
}

int32_t sceVideoOutGetVblankStatus(int32_t handle, SceVideoOutVblankStatus *status) {
    if (!status) return -EINVAL;

    pthread_mutex_lock(&g_video_table_mutex);
    VideoOutHandle *vh = find_video_handle_locked(handle);
    if (!vh) {
        pthread_mutex_unlock(&g_video_table_mutex);
        return -EINVAL;
    }

    pthread_mutex_lock(&vh->mutex);
    *status = vh->vblankStatus;
    pthread_mutex_unlock(&vh->mutex);
    pthread_mutex_unlock(&g_video_table_mutex);
    return 0;
}

int32_t sceVideoOutGetDeviceCapabilityInfo(int32_t handle, SceVideoOutDeviceCapabilityInfo *info) {
    (void)handle;
    if (!info) return -EINVAL;
    info->capability = 0;
    return 0;
}

int32_t sceVideoOutGetEventCount(const OrbisKernelEvent *ev) {
    if (!ev || ev->filter != ORBIS_KERNEL_EVFILT_VIDEO_OUT) return -EINVAL;
    return (int32_t)((ev->data >> 12) & 0xf);
}

int32_t sceVideoOutGetEventId(const OrbisKernelEvent *ev) {
    if (!ev || ev->filter != ORBIS_KERNEL_EVFILT_VIDEO_OUT) return -EINVAL;
    switch ((int32_t)ev->ident) {
    case 0x6: // OrbisVideoOutInternalEventId::Flip
        return 0; // SCE_VIDEO_OUT_EVENT_FLIP
    case 0x7: // OrbisVideoOutInternalEventId::Vblank
    case 0x57: // SysVblank
        return 1; // SCE_VIDEO_OUT_EVENT_VBLANK
    case 0x53: // PreVblankStart
        return 2;
    case 0x51: // SetMode
        return 8;
    case 0x54: // Position
        return 12;
    default:
        return -EINVAL;
    }
}

void shim_sceVideoOutAddVblankEvent(GuestContext *ctx) {
    OrbisKernelEqueue eq = (OrbisKernelEqueue)ctx->rdi;
    int32_t handle = (int32_t)ctx->rsi;
    // udata is returned to the guest verbatim, so keep the guest value.
    void *udata = (void *)(uintptr_t)ctx->rdx;
    int32_t rc = sceVideoOutAddVblankEvent(eq, handle, udata);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutDeleteVblankEvent(GuestContext *ctx) {
    OrbisKernelEqueue eq = (OrbisKernelEqueue)ctx->rdi;
    int32_t handle = (int32_t)ctx->rsi;
    int32_t rc = sceVideoOutDeleteVblankEvent(eq, handle);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutWaitVblank(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    int32_t rc = sceVideoOutWaitVblank(handle);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutGetVblankStatus(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    uint64_t statusGuest = ctx->rsi;
    SceVideoOutVblankStatus *status = statusGuest ? (SceVideoOutVblankStatus *)(ctx->mem_base + statusGuest) : NULL;
    int32_t rc = sceVideoOutGetVblankStatus(handle, status);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutGetDeviceCapabilityInfo(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    uint64_t infoGuest = ctx->rsi;
    SceVideoOutDeviceCapabilityInfo *info = infoGuest ? (SceVideoOutDeviceCapabilityInfo *)(ctx->mem_base + infoGuest) : NULL;
    int32_t rc = sceVideoOutGetDeviceCapabilityInfo(handle, info);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutGetDeviceCapabilityInfo_(GuestContext *ctx) {
    shim_sceVideoOutGetDeviceCapabilityInfo(ctx);
}

void shim_sceVideoOutGetEventCount(GuestContext *ctx) {
    uint64_t evGuest = ctx->rdi;
    const OrbisKernelEvent *ev = evGuest ? (const OrbisKernelEvent *)(ctx->mem_base + evGuest) : NULL;
    int32_t rc = sceVideoOutGetEventCount(ev);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutGetEventId(GuestContext *ctx) {
    uint64_t evGuest = ctx->rdi;
    const OrbisKernelEvent *ev = evGuest ? (const OrbisKernelEvent *)(ctx->mem_base + evGuest) : NULL;
    int32_t rc = sceVideoOutGetEventId(ev);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutModeSetAny_(GuestContext *ctx) {
    uint64_t modeGuest = ctx->rdi;
    uint32_t size = (uint32_t)ctx->rsi;
    void *mode = modeGuest ? (void *)(ctx->mem_base + modeGuest) : NULL;
    sceVideoOutModeSetAny_(mode, size);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceVideoOutConfigureOutputMode_(GuestContext *ctx) {
    int32_t handle = (int32_t)ctx->rdi;
    uint32_t reserved = (uint32_t)ctx->rsi;
    uint64_t modeGuest = ctx->rdx;
    uint64_t optionsGuest = ctx->rcx;
    uint32_t size_mode = (uint32_t)ctx->r8;
    uint32_t size_options = (uint32_t)ctx->r9;

    const void *mode = modeGuest ? (const void *)(ctx->mem_base + modeGuest) : NULL;
    const void *options = optionsGuest ? (const void *)(ctx->mem_base + optionsGuest) : NULL;

    int32_t rc = sceVideoOutConfigureOutputMode_(handle, reserved, mode, options, size_mode, size_options);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutConfigureOptionsInitialize_(GuestContext *ctx) {
    uint64_t optionsGuest = ctx->rdi;
    uint32_t size = (uint32_t)ctx->rsi;
    void *options = optionsGuest ? (void *)(ctx->mem_base + optionsGuest) : NULL;
    sceVideoOutConfigureOptionsInitialize_(options, size);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceVideoOutDeleteFlipEvent(GuestContext *ctx) {
    OrbisKernelEqueue eq = (OrbisKernelEqueue)ctx->rdi;
    int32_t handle = (int32_t)ctx->rsi;
    int32_t rc = sceVideoOutDeleteFlipEvent(eq, handle);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceVideoOutGetEventData(GuestContext *ctx) {
    uint64_t evGuest = ctx->rdi;
    uint64_t dataGuest = ctx->rsi;
    const OrbisKernelEvent *ev = evGuest ? (const OrbisKernelEvent *)(ctx->mem_base + evGuest) : NULL;
    int64_t *data = dataGuest ? (int64_t *)(ctx->mem_base + dataGuest) : NULL;
    int32_t rc = sceVideoOutGetEventData(ev, data);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// Output color settings. The game's brightness option ends up here: it fills the settings
// with sceVideoOutColorSettingsSetGamma and applies them with sceVideoOutAdjustColor. The
// presenter applies the gamma when it copies the frame to the window.
typedef struct {
    float gamma[3];
    uint32_t reserved[3];
} OrbisVideoOutColorSettings;

#define ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE 0x80290001u
#define ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS 0x80290002u

void shim_sceVideoOutColorSettingsSetGamma_(GuestContext *ctx) {
    const uint64_t guest = ctx->rdi;
    const float gamma = ctx->xmm[0].f32[0];
    if (!guest) {
        ctx->rax = ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    } else if (!(gamma >= 0.1f && gamma <= 2.0f)) {
        ctx->rax = ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    } else {
        OrbisVideoOutColorSettings *s = (OrbisVideoOutColorSettings *)(ctx->mem_base + guest);
        s->gamma[0] = s->gamma[1] = s->gamma[2] = gamma;
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_sceVideoOutAdjustColor_(GuestContext *ctx) {
    const int32_t handle = (int32_t)ctx->rdi;
    const uint64_t guest = ctx->rsi;
    if (!guest) {
        ctx->rax = ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
        SHIM_RETURN();
        return;
    }
    const OrbisVideoOutColorSettings *s = (const OrbisVideoOutColorSettings *)(ctx->mem_base + guest);
    fprintf(stderr, "[videoout] AdjustColor(handle=%d, gamma=%.3f %.3f %.3f)\n", handle, s->gamma[0], s->gamma[1],
            s->gamma[2]);
    ps4_metal_screen_set_gamma(s->gamma[0]);
    ctx->rax = 0;
    SHIM_RETURN();
}
