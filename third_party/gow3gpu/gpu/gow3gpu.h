/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef GOW3GPU_H
#define GOW3GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} Gow3GpuConfig;

/* Versioned guest-memory contract. Addresses passed here are PS4 guest virtual
 * addresses, never host pointers. The host owns the mapping and must validate
 * full ranges while holding its VM/direct-memory synchronization. The callbacks
 * must be safe from GPU worker threads. */
#define GOW3GPU_GUEST_MEMORY_ABI_VERSION 1u
typedef void (*Gow3GpuGuestRangeHook)(void *hook_context, uint64_t address,
                                      uint64_t size);
typedef enum {
    GOW3GPU_ATOMIC_FETCH_ADD = 0,
    GOW3GPU_ATOMIC_FETCH_SUB = 1,
    GOW3GPU_ATOMIC_FETCH_SUB_IF_NONZERO = 2,
    GOW3GPU_ATOMIC_STORE_RELEASE = 3,
} Gow3GpuAtomicU64Operation;
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    void *user_context;
    /* True only when the complete requested range is GPU-mappable. */
    int (*is_mapped)(void *user_context, uint64_t address, uint64_t size);
    /* Convert a host pointer from the runtime's active guest mapping to its
     * guest VA. Used to map host access faults back into GPU page tracking. */
    int (*host_to_guest)(void *user_context, const void *host_pointer,
                         uint64_t *guest_address);
    /* Return a contiguous readable extent or hole containing address. The
     * half-open [region_start, region_end) must contain address. mapped=0
     * denotes a sparse hole, which the renderer reads as zeroes. */
    int (*query_region)(void *user_context, uint64_t address,
                        uint64_t *region_start, uint64_t *region_end,
                        int *mapped);
    /* Copy exactly size bytes; false means the entire copy was rejected. */
    int (*read)(void *user_context, uint64_t address, void *destination,
                uint64_t size);
    /* Acquire-ordered aligned atomic load of 1, 4, or 8 bytes for PM4 waits. */
    int (*atomic_load)(void *user_context, uint64_t address, uint32_t size,
                       uint64_t *value);
    /* Atomic semaphore update; fetch operations use acquire-release ordering. */
    int (*atomic_u64)(void *user_context, uint64_t address, uint32_t operation,
                      uint64_t operand, uint64_t *previous_value);
    /* Publish GPU-produced bytes to guest backing memory after the renderer's
     * required GPU completion point; preserve host page-tracking ownership. */
    int (*writeback)(void *user_context, uint64_t address, const void *source,
                     uint64_t size);
    uint64_t (*clamp_range_size)(void *user_context, uint64_t address,
                                 uint64_t requested_size);
    /* Apply GPU page-tracking protections without changing guest VM rights. */
    int (*gpu_protect)(void *user_context, uint64_t address, uint64_t size,
                       int read, int write);
    /* Install callbacks used by the GPU when guest mappings change. */
    int (*install_gpu_hooks)(void *user_context,
                             Gow3GpuGuestRangeHook map,
                             Gow3GpuGuestRangeHook unmap,
                             Gow3GpuGuestRangeHook invalidate,
                             void *hook_context);
} Gow3GpuGuestMemoryApi;

/* Install exactly once before gow3gpu_init. Invalid or incomplete contracts
 * are rejected before any renderer/window construction begins. */
int gow3gpu_set_guest_memory_api(const Gow3GpuGuestMemoryApi *api);

/* Versioned bridge to host-owned runtime services. These callbacks are used by
 * the imported GPU/HLE code instead of importing a second PS4 runtime. errno_address
 * returns this host thread's C-library errno slot; it is not the guest's PS4 errno
 * field. The host owns VFS path translation, clocks, thread bookkeeping and restart
 * policy. */
#define GOW3GPU_HOST_RUNTIME_ABI_VERSION 1u
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    void *user_context;
    int32_t *(*errno_address)(void *user_context);
    uint64_t (*process_time_us)(void *user_context);
    uint64_t (*process_time_counter)(void *user_context);
    uint64_t (*tsc_frequency)(void *user_context);
    int (*translate_path)(void *user_context, const char *guest_path,
                          char *host_path, uint64_t host_path_size);
    int (*attach_host_thread)(void *user_context, const char *name);
    void (*detach_host_thread)(void *user_context);
    void (*request_restart)(void *user_context);
} Gow3GpuHostRuntimeApi;

/* Install exactly once before gow3gpu_init. Every callback is required. */
int gow3gpu_set_host_runtime_api(const Gow3GpuHostRuntimeApi *api);
int gow3gpu_host_runtime_api_is_installed(void);
/* Registers kernel event queues (needed with or without graphics). */
void gow3gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int gow3gpu_init(const Gow3GpuConfig *config);
/* Diagnostic queue drain. This waits for the candidate processor to retire
 * submitted work; guest EOP/equeue semantics must still use their own bridge. */
int gow3gpu_wait_idle(void);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t gow3gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int gow3gpu_handle_fault(void *ucontext, void *address);
/* GOW3_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void gow3gpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int gow3gpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int gow3gpu_overlay_captures_input(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned gow3gpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
