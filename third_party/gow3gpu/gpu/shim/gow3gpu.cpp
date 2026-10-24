#include "gow3_write_log.h"
// gow3: glue between the C loader and the vendored shadPS4 video core.
#include <xxhash.h>
#include "gow3_overlay.h"
#include "gow3_png.h"
#include "gow3_settings.h"
#include "gow3_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "gow3_toggles.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif
#include "../gow3gpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "common/scope_exit.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
std::mutex g_guest_memory_api_mutex;
Gow3GpuGuestMemoryApi g_guest_memory_api{};
std::atomic_bool g_guest_memory_api_installed{};
std::atomic_bool g_guest_memory_error_reported{};

bool GetGuestMemoryApi(Gow3GpuGuestMemoryApi& api) {
    // The setter publishes this table once and rejects all replacements. An acquire
    // makes the immutable table visible without locking on every WAIT_REG_MEM poll,
    // shader fetch and resource query (the GPU threads queued on this mutex in gameplay).
    // GOW3_LOCK_GUEST_MEMORY_API=1 restores the lock for controlled A/B runs.
    static const bool lock_reads = [] {
        const char* value = std::getenv("GOW3_LOCK_GUEST_MEMORY_API");
        return value && value[0] == '1';
    }();
    std::unique_lock lock{g_guest_memory_api_mutex, std::defer_lock};
    if (lock_reads) lock.lock();
    if (!g_guest_memory_api_installed.load(std::memory_order_acquire)) return false;
    api = g_guest_memory_api;
    return true;
}

bool GuestMemoryApiInstalled() {
    return g_guest_memory_api_installed.load(std::memory_order_acquire);
}

void ReportGuestMemoryError(const char* operation, VAddr address, u64 size) {
    if (!g_guest_memory_error_reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[gow3gpu-memory] %s failed for guest range %#llx+%#llx; "
                     "subsequent errors are suppressed\n",
                     operation, static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(size));
    }
}
} // namespace

extern "C" int gow3gpu_set_guest_memory_api(const Gow3GpuGuestMemoryApi* api) {
    if (!api || api->abi_version != GOW3GPU_GUEST_MEMORY_ABI_VERSION ||
        api->struct_size < sizeof(Gow3GpuGuestMemoryApi) || !api->is_mapped ||
        !api->host_to_guest || !api->query_region || !api->read || !api->atomic_load ||
        !api->atomic_u64 || !api->writeback ||
        !api->clamp_range_size || !api->gpu_protect || !api->install_gpu_hooks ||
        !api->allocate_process_memory || !api->release_process_memory) {
        return -1;
    }
    std::scoped_lock lock{g_guest_memory_api_mutex};
    if (g_guest_memory_api_installed.load(std::memory_order_relaxed)) return -2;
    g_guest_memory_api = *api;
    g_guest_memory_api_installed.store(true, std::memory_order_release);
    return 0;
}

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const Gow3GpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    if (!rasterizer_) {
        return;
    }
    Gow3GpuGuestMemoryApi api{};
    if (!GetGuestMemoryApi(api) ||
        !api.install_gpu_hooks(api.user_context,
        [](void* context, uint64_t address, uint64_t size) {
            auto* r = static_cast<Vulkan::Rasterizer*>(context);
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](void* context, uint64_t address, uint64_t size) {
            static_cast<Vulkan::Rasterizer*>(context)->UnmapMemory(address, size);
        },
        [](void* context, uint64_t address, uint64_t size) {
            static_cast<Vulkan::Rasterizer*>(context)->InvalidateMemory(address, size);
        }, rasterizer_)) {
        ReportGuestMemoryError("install GPU mapping hooks", 0, 0);
    }
}
bool MemoryManager::IsValidGpuMapping(VAddr virtual_addr, u64 size) {
    Gow3GpuGuestMemoryApi api{};
    return GetGuestMemoryApi(api) && api.is_mapped(api.user_context, virtual_addr, size) != 0;
}
bool MemoryManager::GuestAddressFromHost(const void* host_pointer, VAddr& guest_address) {
    Gow3GpuGuestMemoryApi api{};
    guest_address = 0;
    uint64_t address = 0;
    if (!host_pointer || !GetGuestMemoryApi(api) ||
        !api.host_to_guest(api.user_context, host_pointer, &address)) {
        return false;
    }
    guest_address = static_cast<VAddr>(address);
    return true;
}
bool MemoryManager::AllocateProcessMemory(u64 size, u64 alignment, const char* name,
                                          VAddr& guest_address, void*& host_pointer) {
    Gow3GpuGuestMemoryApi api{};
    guest_address = 0;
    host_pointer = nullptr;
    if (!size || !alignment || (alignment & (alignment - 1)) ||
        !GetGuestMemoryApi(api)) return false;
    uint64_t api_guest_address = 0;
    if (!api.allocate_process_memory(api.user_context, size, alignment, name,
                                     &api_guest_address, &host_pointer)) return false;
    uint64_t translated_address = 0;
    if (!host_pointer || !api.is_mapped(api.user_context, api_guest_address, size) ||
        !api.host_to_guest(api.user_context, host_pointer, &translated_address) ||
        translated_address != api_guest_address) {
        if (host_pointer) {
            api.release_process_memory(api.user_context, api_guest_address, size,
                                       host_pointer);
        }
        host_pointer = nullptr;
        return false;
    }
    guest_address = static_cast<VAddr>(api_guest_address);
    return true;
}
bool MemoryManager::ReleaseProcessMemory(VAddr guest_address, u64 size,
                                        void* host_pointer) {
    Gow3GpuGuestMemoryApi api{};
    return guest_address && size && host_pointer && GetGuestMemoryApi(api) &&
           api.release_process_memory(api.user_context, guest_address, size,
                                     host_pointer) != 0;
}
bool MemoryManager::AtomicLoad(VAddr address, u32 size, u64& value) {
    Gow3GpuGuestMemoryApi api{};
    value = 0;
    if ((size != 1 && size != 4 && size != 8) || address % size != 0 ||
        !GetGuestMemoryApi(api) ||
        !api.atomic_load(api.user_context, address, size, &value)) {
        ReportGuestMemoryError("atomic guest load", address, size);
        return false;
    }
    return true;
}
bool MemoryManager::AtomicSemaphore(VAddr address, u32 operation, u64 operand,
                                    u64& previous_value) {
    Gow3GpuGuestMemoryApi api{};
    previous_value = 0;
    if ((address & (alignof(u64) - 1)) != 0 || operation > GOW3GPU_ATOMIC_STORE_RELEASE ||
        !GetGuestMemoryApi(api) ||
        !api.atomic_u64(api.user_context, address, operation, operand,
                        &previous_value)) {
        ReportGuestMemoryError("atomic guest semaphore", address, sizeof(u64));
        return false;
    }
    return true;
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    Gow3GpuGuestMemoryApi api{};
    return GetGuestMemoryApi(api)
        ? api.clamp_range_size(api.user_context, virtual_addr, size)
        : 0;
}
static bool CopySparseSerial(const Gow3GpuGuestMemoryApi& api, VAddr source,
                             u8* dest, u64 size) {
    if (!dest || source > std::numeric_limits<VAddr>::max() - size) {
        ReportGuestMemoryError("validate sparse read", source, size);
        if (dest) std::memset(dest, 0, size);
        return false;
    }
    // A range inside one mapped extent (nearly every descriptor, user-data and shader read)
    // reads without the VM lock: query_region takes it on each call, and the command thread,
    // the draw preparation workers and the recording thread queued on it for most of a frame.
    // The walk below handles ranges that cross unmapped or separately mapped regions.
    if (size && api.read(api.user_context, source, dest, size)) {
        return true;
    }
    bool success = true;
    while (size) {
        uint64_t start = 0, end = 0;
        int mapped = 0;
        if (!api.query_region(api.user_context, source, &start, &end, &mapped) ||
            start > source || end <= source) {
            ReportGuestMemoryError("query sparse read", source, size);
            std::memset(dest, 0, size);
            return false;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) {
            if (!api.read(api.user_context, source, dest, n)) {
                ReportGuestMemoryError("read guest bytes", source, n);
                std::memset(dest, 0, n);
                success = false;
            }
        } else {
            std::memset(dest, 0, n);
        }
        source += n; dest += n; size -= n;
    }
    return success;
}
bool MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    Gow3GpuGuestMemoryApi api{};
    if (!GetGuestMemoryApi(api)) {
        ReportGuestMemoryError("missing guest-memory API", source, size);
        if (dest) std::memset(dest, 0, size);
        return false;
    }
    if (!dest || source > std::numeric_limits<VAddr>::max() - size) {
        ReportGuestMemoryError("validate guest read", source, size);
        if (dest) std::memset(dest, 0, size);
        return false;
    }
    // gow3: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !Gow3Copy::Enabled()) {
        return CopySparseSerial(api, source, dest, size);
    }
    std::atomic_bool success{true};
    Gow3Copy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        if (!CopySparseSerial(api, source + offset, dest + offset,
                              std::min(Chunk, size - offset))) {
            success.store(false, std::memory_order_relaxed);
        }
    });
    return success.load(std::memory_order_relaxed);
}
bool MemoryManager::HashSparseMemory(VAddr source, u64 size, u64 seed, u64& hash) {
    if (source > std::numeric_limits<VAddr>::max() - size) {
        ReportGuestMemoryError("validate sparse hash", source, size);
        hash = 0;
        return false;
    }
    XXH3_state_t* state = XXH3_createState();
    if (!state) {
        ReportGuestMemoryError("allocate sparse hash state", source, size);
        hash = 0;
        return false;
    }
    SCOPE_EXIT { XXH3_freeState(state); };
    if (XXH3_64bits_reset_withSeed(state, seed) != XXH_OK) {
        ReportGuestMemoryError("initialize sparse hash state", source, size);
        hash = 0;
        return false;
    }

    std::array<u8, 64 * 1024> chunk{};
    bool success = true;
    for (u64 offset = 0; offset < size;) {
        const u64 count = std::min<u64>(chunk.size(), size - offset);
        if (!CopySparseMemory(source + offset, chunk.data(), count)) {
            success = false;
        }
        if (XXH3_64bits_update(state, chunk.data(), count) != XXH_OK) {
            ReportGuestMemoryError("update sparse hash state", source + offset, count);
            hash = 0;
            return false;
        }
        offset += count;
    }
    hash = XXH3_64bits_digest(state);
    return success;
}
bool MemoryManager::TryWriteBacking(VAddr guest_address, const void* data, u64 size) {
    Gow3WriteLog::Note(guest_address, data, size, Gow3WriteLog::Backing);
    Gow3GpuGuestMemoryApi api{};
    const bool written = GetGuestMemoryApi(api) &&
                         api.writeback(api.user_context, guest_address, data, size) != 0;
    // Upstream a101edb (#331): the write skipped page tracking unless it happened to fault,
    // so textures over it must reload. Fence and label writes (<= 8 bytes) never hold texels;
    // noting them would send every texture binding through the locked path.
    if (written && rasterizer && size > 8) {
        rasterizer->GetTextureCache().NoteBackingWrite(guest_address, size);
    }
    return written;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    Gow3Stats::Timer timer{Gow3Stats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    Gow3Stats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    Gow3Stats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        Gow3Stats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        Gow3Stats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    Gow3GpuGuestMemoryApi api{};
    if (!GetGuestMemoryApi(api) ||
        !api.gpu_protect(api.user_context, virtual_addr, size,
                         True(perms & MemoryPermission::Read),
                         True(perms & MemoryPermission::Write))) {
        ReportGuestMemoryError("apply GPU page protection", virtual_addr, size);
    }
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// Non-Apple SDL platforms keep their dedicated window thread. Cocoa objects and
// SDL event polling are owned by the process main thread on macOS.
#ifdef __APPLE__
std::mutex g_window_mutex;
dispatch_source_t g_window_event_source{};
std::atomic<u32> g_display_refresh_hz{60};

static void UpdateDisplayRefreshHzOnWindowThread() {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (mode && mode->refresh_rate > 0) {
        g_display_refresh_hz.store(u32(mode->refresh_rate + 0.5f), std::memory_order_relaxed);
    }
}

struct WindowCreateRequest {
    const std::string* title;
    s32 width;
    s32 height;
    int result{-1};
};

Frontend::WindowSDL* GetWindow() {
    std::scoped_lock lock{g_window_mutex};
    return g_window;
}

static void PumpWindowEvents(void* context) {
    auto* window = static_cast<Frontend::WindowSDL*>(context);
    if (window && !window->PollEvents()) {
        LOG_INFO(Frontend, "Window closed by user");
        std::fflush(stdout);
        std::_Exit(0);
    }
}

static void CreateWindowOnMainThread(void* context) {
    auto* request = static_cast<WindowCreateRequest*>(context);
    auto* window = new Frontend::WindowSDL(request->width, request->height,
                                           request->title->c_str());
    UpdateDisplayRefreshHzOnWindowThread();
    {
        std::scoped_lock lock{g_window_mutex};
        g_window = window;
    }
    g_window_event_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                    dispatch_get_main_queue());
    if (!g_window_event_source) {
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = nullptr;
        }
        delete window;
        request->result = -1;
        return;
    }
    dispatch_source_set_event_handler_f(g_window_event_source, &PumpWindowEvents);
    dispatch_set_context(g_window_event_source, window);
    dispatch_source_set_timer(g_window_event_source, DISPATCH_TIME_NOW,
                              5 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_resume(g_window_event_source);
    request->result = 0;
}

static int CreateWindow(const std::string& title, s32 width, s32 height) {
    WindowCreateRequest request{&title, width, height, -1};
    if (pthread_main_np()) {
        CreateWindowOnMainThread(&request);
    } else {
        // ps4_app keeps its Cocoa event loop on the process main thread while
        // recompiled guest code runs on a worker. Synchronous dispatch creates
        // the NSWindow and CAMetalLayer before Vulkan builds the surface.
        dispatch_sync_f(dispatch_get_main_queue(), &request, &CreateWindowOnMainThread);
    }
    return request.result;
}
#else
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
std::atomic<u32> g_display_refresh_hz{60};
static void UpdateDisplayRefreshHzOnWindowThread() {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (mode && mode->refresh_rate > 0) {
        g_display_refresh_hz.store(u32(mode->refresh_rate + 0.5f), std::memory_order_relaxed);
    }
}
Frontend::WindowSDL* GetWindow() {
    std::scoped_lock lock{g_window_mutex};
    return g_window;
}
#endif
} // namespace

u32 Gow3DisplayRefreshHz() {
    return std::max<u32>(g_display_refresh_hz.load(std::memory_order_relaxed), 60);
}

#ifdef GOW3_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (GOW3_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("gow3:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int gow3gpu_init(const Gow3GpuConfig* config) {
    if (!config || !GuestMemoryApiInstalled() || !gow3gpu_host_runtime_api_is_installed()) {
        std::fprintf(stderr,
                     "[gow3gpu] init rejected: guest-memory and host-runtime APIs must be installed first\n");
        return -1;
    }
    Gow3Settings::Load();
#ifdef GOW3_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("GOW3_GPU_USER_DIR")) _putenv_s("GOW3_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("GOW3_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "God of War III";
    const s32 width = config->width > 0 ? config->width : 1920;
    const s32 height = config->height > 0 ? config->height : 1080;
#ifdef __APPLE__
    if (CreateWindow(title, width, height) != 0) {
        std::fprintf(stderr, "[gow3gpu] failed to create SDL Cocoa window on the main thread\n");
        return -1;
    }
#else
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("gow3:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        UpdateDisplayRefreshHzOnWindowThread();
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
#endif
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void gow3gpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t gow3gpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int gow3gpu_handle_fault(void* ucontext, void* address) {
    if (runtime_fault_recover) {
        sigjmp_buf* recover = runtime_fault_recover;
        runtime_fault_recover = nullptr;
        siglongjmp(*recover, 1);
    }
    if (!ucontext || !address) return 0;
    auto* signals = Core::Signals::Instance();
    return signals && signals->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned gow3gpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("gow3:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int gow3gpu_overlay_captures_input(void) {
    return Gow3Overlay::CapturesInput() ? 1 : 0;
}

extern "C" int gow3gpu_text_input_begin(const char* initial, const char* prompt) {
    auto* window = GetWindow();
    if (!window) return 0;
    window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int gow3gpu_choice_begin(const char* title, const char* const* items, const char* const* images,
                                    int count, int focus) {
    if (!GetWindow() || count <= 0) return 0;
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<Gow3Png::Image> pictures(count);
    for (int i = 0; images && i < count; ++i) {
        if (images[i] && !Gow3Png::LoadRgba(images[i], pictures[i])) {
            LOG_WARNING(Lib_SaveDataDialog, "save list: no picture from {}", images[i]);
        }
    }
    Gow3Overlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                             std::move(pictures), focus);
    return 1;
}

extern "C" int gow3gpu_choice_poll(void) {
    return Gow3Overlay::PollChoice();
}

extern "C" int gow3gpu_text_input_poll(char* out, uint64_t size) {
    auto* window = GetWindow();
    if (!window) return 2;
    std::string text;
    const int state = window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
