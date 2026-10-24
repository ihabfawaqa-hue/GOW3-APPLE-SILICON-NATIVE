#include "recomp_runtime.h"
#include "gow3gpu_runtime_loader.h"
#include "ps4_vfs.h"
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libgen.h>
#endif

extern void recomp_init_dispatch_table(void);
extern void recomp_enter_guest(GuestContext *ctx, uint64_t target_fn);

typedef struct {
    int argc;
    char **argv;
} GuestMainArgs;

static const char *get_game_path(void) {
    const char *env = getenv("GOW3_DIR");
    if (env && *env) return env;
    env = getenv("PS4_APP_DIR");
    if (env && *env) return env;
    if (access("/Volumes/1TB/gow3_cusa01623_extracted", F_OK) == 0) return "/Volumes/1TB/gow3_cusa01623_extracted";
    if (access("gow3_cusa01623_extracted", F_OK) == 0) return "gow3_cusa01623_extracted";
    if (access("../gow3_cusa01623_extracted", F_OK) == 0) return "../gow3_cusa01623_extracted";
    if (access("./game", F_OK) == 0) return "./game";
    if (access("./app0", F_OK) == 0) return "./app0";
    return "/Volumes/1TB/gow3_cusa01623_extracted";
}

// Guest code runs on a secondary host thread so the host main thread stays free
// for AppKit (window creation, event pumping, dialogs).
static void *guest_main(void *p) {
    GuestMainArgs *a = (GuestMainArgs *)p;
    int argc = a->argc;
    char **argv = a->argv;
    printf("[ps4-recomp] Initializing runtime (ARM64 Assembly Backend)...\n");
    const char *app_dir = get_game_path();
    printf("[ps4-recomp] Using game asset directory: %s\n", app_dir);
    ps4_vfs_init(app_dir);
    recomp_init_dispatch_table();

    const char *prog_name = (argc > 0 && argv[0]) ? argv[0] : "/app0/eboot.bin";
    GuestContext *ctx = recomp_init_runtime_file("guest_image.bin", 0, prog_name);
    if (!ctx) {
        fprintf(stderr, "[ps4-recomp] Failed to allocate guest memory or load guest_image.bin\n");
        exit(1);
    }

    // Runtime initialization: EOP synchronization and IntroLoaderThread completion
    // are now handled authentically in ps4_gnm_metal.mm and runtime shims.
    // Speculative startup patches removed to prevent memory and shader corruption.
    printf("[gow3] Initializing runtime with native ARM64 backend...\n");

    recomp_init_process_param(ctx, 0x3c0000ULL);

    recomp_init_tls(ctx, 0x3c0040ULL, 0x0ULL, 0x2740ULL, 0x10ULL);

    recomp_setup_entry_args(ctx, argc, argv, 0x144070ULL);

    if (ps4_gow3gpu_start_requested_backend(ctx) != 0) {
        fprintf(stderr, "[ps4-recomp] Requested GoW3 Vulkan backend failed to initialize\n");
        exit(1);
    }

    printf("[ps4-recomp] Executing %s (0x%x) via ARM64 assembly...\n", "_start", (unsigned int)0x144070ULL);
    extern void fn_0x144070(void);
    recomp_enter_guest(ctx, (uint64_t)fn_0x144070);

    printf("[ps4-recomp] Execution complete.\n");
    recomp_free_runtime(ctx);
    exit(0);
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef __APPLE__
    char raw_path[1024];
    uint32_t raw_path_size = sizeof(raw_path);
    if (_NSGetExecutablePath(raw_path, &raw_path_size) == 0) {
        char real_exe[1024];
        if (realpath(raw_path, real_exe)) {
            char *dir = dirname(real_exe);
            if (dir && *dir) {
                chdir(dir);
            }
        }
    }
#endif
    static GuestMainArgs args;
    args.argc = argc;
    args.argv = argv;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    // Startup settings (language, PS4 Pro mode, upscaling, FPS HUD) are resolved before the
    // guest starts; the dialog needs the application directory for the language list.
    ps4_vfs_init(get_game_path());
    ps4_host_settings_init();
    pthread_t guest_thread;
    if (pthread_create(&guest_thread, &attr, guest_main, &args) != 0) {
        fprintf(stderr, "[ps4-recomp] Failed to start guest thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    ps4_host_run_main_loop();
    return 0;
}
