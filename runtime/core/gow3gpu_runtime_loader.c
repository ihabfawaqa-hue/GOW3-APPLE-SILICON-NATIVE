// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3gpu_runtime_loader.h"

#include "gow3gpu_gnm_adapter.h"
#include "gow3gpu_runtime_adapter.h"
#include "ps4_sfo.h"
#include "../../third_party/gow3gpu/gpu/gow3gpu.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef int (*SetGuestMemoryApiFn)(const Gow3GpuGuestMemoryApi *api);
typedef int (*SetHostRuntimeApiFn)(const Gow3GpuHostRuntimeApi *api);
typedef int (*CandidateInitFn)(const Gow3GpuConfig *config);
typedef void (*RegisterKernelFn)(void);
typedef uintptr_t (*ResolveNidFn)(const char *scoped_nid);
typedef int (*CandidateFaultFn)(void *ucontext, void *address);

/* The C ABI has no coordinated shutdown yet. Keep this handle loaded for the
 * process lifetime; the bounded lab terminates only the app instance it starts. */
static void *g_candidate_library;
static ResolveNidFn g_candidate_resolve;

uintptr_t ps4_gow3gpu_resolve_candidate(const char *scoped_nid) {
  if (!scoped_nid || !g_candidate_resolve ||
      !ps4_gow3gpu_submission_backend_active())
    return 0;
  return g_candidate_resolve(scoped_nid);
}

static void *required_symbol(void *library, const char *name) {
  dlerror();
  void *symbol = dlsym(library, name);
  const char *error = dlerror();
  if (error || !symbol) {
    fprintf(stderr, "[gow3gpu-route] missing export %s: %s\n", name,
            error ? error : "null symbol");
    return NULL;
  }
  return symbol;
}

static int read_proc_param_metadata(GuestContext *ctx, uint32_t *sdk_version) {
  GuestContext *proc = ctx ? (ctx->process_ctx ? ctx->process_ctx : ctx) : NULL;
  if (!proc || !proc->mem_base || !sdk_version ||
      !proc->proc_param_addr || proc->proc_param_addr > proc->mem_size ||
      proc->mem_size - (size_t)proc->proc_param_addr < 0x14)
    return 0;
  uint32_t magic = 0;
  memcpy(&magic, proc->mem_base + proc->proc_param_addr + 8, sizeof(magic));
  if (magic != UINT32_C(0x4942524f)) /* little-endian "ORBI" */
    return 0;
  memcpy(sdk_version, proc->mem_base + proc->proc_param_addr + 0x10,
         sizeof(*sdk_version));
  return 1;
}

int ps4_gow3gpu_start_requested_backend(GuestContext *ctx) {
  const char *backend = getenv("PS4_GOW3GPU_BACKEND");
  if (!backend || strcmp(backend, "gow3gpu-vulkan") != 0) return 0;

  const char *library_path = getenv("PS4_GOW3GPU_LIBRARY");
  if (!library_path || !*library_path) {
    fprintf(stderr, "[gow3gpu-route] backend requested without PS4_GOW3GPU_LIBRARY\n");
    return -1;
  }
  if (!ctx || !ctx->mem_base) {
    fprintf(stderr, "[gow3gpu-route] runtime context is not initialized\n");
    return -1;
  }

  const char *user_dir = getenv("PS4_GOW3GPU_USER_DIR");
  if (!user_dir || !*user_dir) user_dir = "/tmp/gow3gpu-user";
  if (mkdir(user_dir, 0700) != 0 && errno != EEXIST) {
    fprintf(stderr, "[gow3gpu-route] cannot create user directory %s: %s\n",
            user_dir, strerror(errno));
    return -1;
  }
  struct stat user_dir_stat;
  if (stat(user_dir, &user_dir_stat) != 0 || !S_ISDIR(user_dir_stat.st_mode)) {
    fprintf(stderr, "[gow3gpu-route] user directory is not a directory: %s\n",
            user_dir);
    return -1;
  }

  g_candidate_library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
  if (!g_candidate_library) {
    fprintf(stderr, "[gow3gpu-route] dlopen failed for %s: %s\n", library_path,
            dlerror());
    return -1;
  }

  SetGuestMemoryApiFn set_memory_api =
      (SetGuestMemoryApiFn)required_symbol(g_candidate_library,
                                          "gow3gpu_set_guest_memory_api");
  SetHostRuntimeApiFn set_host_api =
      (SetHostRuntimeApiFn)required_symbol(g_candidate_library,
                                          "gow3gpu_set_host_runtime_api");
  CandidateInitFn candidate_init =
      (CandidateInitFn)required_symbol(g_candidate_library, "gow3gpu_init");
  RegisterKernelFn register_kernel =
      (RegisterKernelFn)required_symbol(g_candidate_library,
                                        "gow3gpu_register_kernel");
  ResolveNidFn resolve =
      (ResolveNidFn)required_symbol(g_candidate_library, "gow3gpu_resolve");
  CandidateFaultFn handle_fault =
      (CandidateFaultFn)required_symbol(g_candidate_library,
                                        "gow3gpu_handle_fault");
  if (!set_memory_api || !set_host_api || !candidate_init || !register_kernel ||
      !resolve || !handle_fault)
    return -1;

  if (ps4_gow3gpu_install_runtime_apis(ctx, set_memory_api, set_host_api,
                                       handle_fault) != 0) {
    fprintf(stderr, "[gow3gpu-route] primary runtime callbacks were rejected\n");
    return -1;
  }

  uint32_t sdk_version = 0;
  if (!read_proc_param_metadata(ctx, &sdk_version)) {
    fprintf(stderr, "[gow3gpu-route] cannot read SDK version from SceKernelProcParam\n");
    return -1;
  }
  char title[256] = "God of War III Remastered";
  char serial[64] = "CUSA01623";
  uint32_t psf_attributes = 0;
  Ps4SfoContext sfo = {0};
  if (ps4_sfo_find_and_load(&sfo)) {
    int32_t attributes = 0;
    ps4_sfo_get_string(&sfo, "TITLE", title, sizeof(title));
    ps4_sfo_get_string(&sfo, "TITLE_ID", serial, sizeof(serial));
    if (ps4_sfo_get_int(&sfo, "ATTRIBUTE", &attributes))
      psf_attributes = (uint32_t)attributes;
    ps4_sfo_free(&sfo);
  } else {
    fprintf(stderr, "[gow3gpu-route] param.sfo unavailable; using known title metadata\n");
  }

  Gow3GpuConfig config = {
      .title = title,
      .serial = serial,
      .user_dir = user_dir,
      .sdk_version = sdk_version,
      .psf_attributes = psf_attributes,
      .width = 1920,
      .height = 1080,
  };
  const int init_result = candidate_init(&config);
  if (init_result != 0) {
    fprintf(stderr, "[gow3gpu-route] candidate renderer initialization failed: %d\n",
            init_result);
    return -1;
  }
  register_kernel();
  g_candidate_resolve = resolve;

  const uintptr_t submit_address =
      resolve("zwY0YV91TTI#libSceGnmDriver#libSceGnmDriver");
  const uintptr_t submit_done_address =
      resolve("yvZ73uQUqrk#libSceGnmDriver#libSceGnmDriver");
  const uintptr_t wait_idle_address =
      (uintptr_t)required_symbol(g_candidate_library, "gow3gpu_wait_idle");
  if (!submit_address || !submit_done_address || !wait_idle_address) {
    fprintf(stderr, "[gow3gpu-route] candidate GNM submit NIDs are unavailable\n");
    return -1;
  }

  static const char *const required_videoout_nids[] = {
      "Up36PTk687E#libSceVideoOut#libSceVideoOut", /* Open */
      "uquVH4-Du78#libSceVideoOut#libSceVideoOut", /* Close */
      "w3BY+tAEiQY#libSceVideoOut#libSceVideoOut", /* RegisterBuffers */
      "N5KDtkIjjJ4#libSceVideoOut#libSceVideoOut", /* UnregisterBuffers */
      "CBiu4mCE1DA#libSceVideoOut#libSceVideoOut", /* SetFlipRate */
      "U46NwOiJpys#libSceVideoOut#libSceVideoOut", /* SubmitFlip */
  };
  for (size_t i = 0; i < sizeof(required_videoout_nids) / sizeof(required_videoout_nids[0]);
       ++i) {
    if (!resolve(required_videoout_nids[i])) {
      fprintf(stderr, "[gow3gpu-route] candidate VideoOut NID unavailable: %s\n",
              required_videoout_nids[i]);
      return -1;
    }
  }

  const Gow3GpuGuestMemoryApi *memory_api =
      ps4_gow3gpu_runtime_memory_api();
  if (!memory_api ||
      ps4_gow3gpu_install_submission_backend(
          memory_api,
          (Ps4Gow3GpuSubmitCommandBuffersFn)submit_address,
          (Ps4Gow3GpuSubmitDoneFn)submit_done_address,
          (Ps4Gow3GpuWaitIdleFn)wait_idle_address) != 0) {
    fprintf(stderr, "[gow3gpu-route] candidate submission route installation failed\n");
    return -1;
  }

  fprintf(stderr,
          "[gow3gpu-route] active PM4 owner=gow3gpu/shadPS4; candidate Vulkan VideoOut "
          "imports available; primary runtime retains flip-event ownership (SDK=0x%08x)\n",
          sdk_version);
  return 0;
}
