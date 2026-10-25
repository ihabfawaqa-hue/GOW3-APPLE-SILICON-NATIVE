// sceSaveDataDialog: the game's save list, its "saving" progress bar and its messages.
//
// God of War III saves at a Save Altar through a list (mode LIST, type SAVE) and loads
// through one (type LOAD); autosaves show a progress bar that the game closes when the
// data is written. The list is shown with the GPU overlay's picker (arrows/D-pad,
// Enter/Cross, Esc/Circle) and the result names the chosen save directory in the game's
// own result buffers. Messages (overwrite and error notices) answer OK/YES at once: the
// player has already chosen in the picker.
//
// PS4_SAVEDATA_DIALOG_AUTO=<index>|cancel answers every list without the picker (bounded
// lab runs, which cannot press the picker's keys). Without the picker a list is cancelled.
#import <Foundation/Foundation.h>
#include "ps4_savedata_dialog.h"
#include "ps4_savedata.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int ps4_gow3gpu_choice_begin(const char *title, const char *const *items,
                             const char *const *images, int count, int focus);
int ps4_gow3gpu_choice_poll(void);

#define DLG_ERROR_NOT_FINISHED ((int32_t)0x80B80005)
#define DLG_ERROR_INVALID_STATE ((int32_t)0x80B80006)
#define DLG_ERROR_PARAM_INVALID ((int32_t)0x80B8000A)
#define DLG_ERROR_NOT_RUNNING ((int32_t)0x80B8000B)
#define DLG_ERROR_ARG_NULL ((int32_t)0x80B8000D)

#define DLG_BUTTON_INVALID 0
#define DLG_BUTTON_OK 1 // also YES

#define DLG_MAX_DIRS 32
#define DLG_DIRNAME_SIZE 32

// OrbisSaveDataDialogParam: BaseParam (48), size @48, mode @52, dispType @56,
// animParam @64, items @72, userMsgParam @80, sysMsgParam @88, errorCodeParam @96,
// progressBarParam @104, userData @112, optionParam @120.
// OrbisSaveDataDialogItems: userId @0, titleId* @8, dirName* @16 (32-byte names),
// dirNameNum @24, newItem* @32, focusPos @40.
// OrbisSaveDataDialogResult: mode @0, result @4, buttonId @8, dirName* @16, param* @24,
// userData @32.
enum {
    PARAM_MODE = 52, PARAM_TYPE = 56, PARAM_ITEMS = 72, PARAM_USER_DATA = 112,
    PARAM_SIZE = 152,
    ITEMS_USER = 0, ITEMS_DIRS = 16, ITEMS_DIR_COUNT = 24, ITEMS_NEW_ITEM = 32,
    ITEMS_SIZE = 48,
    RESULT_MODE = 0, RESULT_RESULT = 4, RESULT_BUTTON = 8, RESULT_DIR = 16,
    RESULT_PARAM = 24, RESULT_USER_DATA = 32, RESULT_SIZE = 72,
};

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
    OrbisCommonDialogStatus status;
    uint32_t mode, type;
    uint64_t user_data; // guest value, handed back unchanged
    int32_t user_id;
    char dirs[DLG_MAX_DIRS][DLG_DIRNAME_SIZE];
    int dir_count, has_new;
    int choosing; // the overlay picker is open
    uint32_t result, button;
    char chosen[DLG_DIRNAME_SIZE]; // "" = the new item
} g_dlg;

static uint8_t *guest_bytes(GuestContext *ctx, uint64_t addr, uint64_t size) {
    if (!ctx || !ctx->mem_base || !addr || addr >= ctx->mem_size || size > ctx->mem_size - addr)
        return NULL;
    return ctx->mem_base + addr;
}

static uint32_t read_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t read_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static const char *type_name(uint32_t type) {
    return type == ORBIS_SAVE_DATA_DIALOG_TYPE_SAVE   ? "Save game"
           : type == ORBIS_SAVE_DATA_DIALOG_TYPE_LOAD ? "Load game"
                                                      : "Delete save";
}

// "(Auto Save) Mount Olympus", the detail lines ("Difficulty: ...", "Play time: ...") and the
// date. A save chosen in a save list is overwritten, which the date line says.
static void slot_label(const char *dir, const Ps4SaveDataParam *p, bool overwrite,
                       char *out, size_t size) {
    char heading[160], detail[200], when[64] = "";
    snprintf(heading, sizeof(heading), "%.120s", p->subTitle[0] ? p->subTitle : p->title[0] ? p->title : dir);
    snprintf(detail, sizeof(detail), "%.190s", p->detail);
    for (char *c = detail; *c; ++c)
        if (*c == '\r') *c = ' ';
    // At most two detail lines beside the 228x128 picture.
    char *nl = strchr(detail, '\n');
    if (nl && (nl = strchr(nl + 1, '\n'))) *nl = '\0';
    if (p->mtime > 0) {
        time_t t = (time_t)p->mtime;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof(when), "Saved %Y-%m-%d %H:%M", &tm);
    }
    snprintf(out, size, "%s%s%s%s%s%s", heading, detail[0] ? "\n" : "", detail,
             when[0] ? "\n" : "", when, overwrite ? "  (choosing it overwrites)" : "");
}

static void finish(uint32_t result, uint32_t button) {
    g_dlg.choosing = 0;
    g_dlg.result = result;
    g_dlg.button = button;
    g_dlg.status = ORBIS_COMMON_DIALOG_STATUS_FINISHED;
}

static void choose(int choice) {
    if (choice < 0) {
        finish(ORBIS_COMMON_DIALOG_RESULT_USER_CANCELED, DLG_BUTTON_INVALID);
        fprintf(stderr, "[savedata-dlg] list (%s): cancelled\n", type_name(g_dlg.type));
        return;
    }
    const int dir = choice - g_dlg.has_new;
    if (dir >= g_dlg.dir_count) {
        finish(ORBIS_COMMON_DIALOG_RESULT_USER_CANCELED, DLG_BUTTON_INVALID);
        return;
    }
    memset(g_dlg.chosen, 0, sizeof(g_dlg.chosen));
    if (dir >= 0) memcpy(g_dlg.chosen, g_dlg.dirs[dir], DLG_DIRNAME_SIZE - 1);
    finish(ORBIS_COMMON_DIALOG_RESULT_OK, DLG_BUTTON_OK);
    fprintf(stderr, "[savedata-dlg] list (%s): chose %s\n", type_name(g_dlg.type),
            g_dlg.chosen[0] ? g_dlg.chosen : "a new save");
}

static int auto_choice(int *choice) {
    const char *env = getenv("PS4_SAVEDATA_DIALOG_AUTO");
    if (!env || !*env) return 0;
    *choice = strcmp(env, "cancel") == 0 ? -2 : atoi(env);
    return 1;
}

static void open_list(GuestContext *ctx, const uint8_t *param) {
    const uint8_t *items = guest_bytes(ctx, read_u64(param + PARAM_ITEMS), ITEMS_SIZE);
    if (items) {
        g_dlg.user_id = (int32_t)read_u32(items + ITEMS_USER);
        const uint32_t count = read_u32(items + ITEMS_DIR_COUNT);
        const uint8_t *names = count ? guest_bytes(ctx, read_u64(items + ITEMS_DIRS),
                                                   (uint64_t)count * DLG_DIRNAME_SIZE)
                                     : NULL;
        for (uint32_t i = 0; names && i < count && g_dlg.dir_count < DLG_MAX_DIRS; ++i) {
            const char *name = (const char *)names + i * DLG_DIRNAME_SIZE;
            if (name[0]) memcpy(g_dlg.dirs[g_dlg.dir_count++], name, DLG_DIRNAME_SIZE - 1);
        }
        g_dlg.has_new = read_u64(items + ITEMS_NEW_ITEM) != 0;
    }

    static char labels[DLG_MAX_DIRS + 1][400], pictures[DLG_MAX_DIRS + 1][1100];
    const char *label_ptrs[DLG_MAX_DIRS + 1], *picture_ptrs[DLG_MAX_DIRS + 1];
    int n = 0, focus = 0;
    int64_t newest = -1;
    if (g_dlg.has_new) {
        snprintf(labels[n], sizeof(labels[n]), "New save");
        label_ptrs[n] = labels[n];
        picture_ptrs[n] = NULL;
        ++n;
    }
    for (int i = 0; i < g_dlg.dir_count; ++i, ++n) {
        Ps4SaveDataParam p;
        memset(&p, 0, sizeof(p));
        ps4_savedata_read_dir_info(g_dlg.user_id, g_dlg.dirs[i], &p, NULL);
        slot_label(g_dlg.dirs[i], &p, g_dlg.type == ORBIS_SAVE_DATA_DIALOG_TYPE_SAVE, labels[n],
                   sizeof(labels[n]));
        label_ptrs[n] = labels[n];
        picture_ptrs[n] = NULL;
        char dir_path[1024];
        if (ps4_savedata_dir_path(g_dlg.user_id, g_dlg.dirs[i], dir_path, sizeof(dir_path))) {
            snprintf(pictures[n], sizeof(pictures[n]), "%s/sce_sys/icon0.png", dir_path);
            if (access(pictures[n], R_OK) == 0) picture_ptrs[n] = pictures[n];
        }
        // Loading starts on the newest save.
        if (g_dlg.type == ORBIS_SAVE_DATA_DIALOG_TYPE_LOAD && p.mtime > newest) {
            newest = p.mtime;
            focus = n;
        }
    }
    // F9 (quick load) asked for the quick save.
    const char *quick = g_dlg.type == ORBIS_SAVE_DATA_DIALOG_TYPE_LOAD ? ps4_savedata_quick_load_focus() : NULL;
    for (int i = 0; quick && i < g_dlg.dir_count; ++i)
        if (!strcmp(g_dlg.dirs[i], quick)) focus = i + g_dlg.has_new;

    int choice;
    if (auto_choice(&choice)) {
        fprintf(stderr, "[savedata-dlg] list (%s): %d entries, answered %d by PS4_SAVEDATA_DIALOG_AUTO\n",
                type_name(g_dlg.type), n, choice);
        choose(choice >= n ? -2 : choice);
        return;
    }
    if (!n || !ps4_gow3gpu_choice_begin(type_name(g_dlg.type), label_ptrs, picture_ptrs, n, focus)) {
        fprintf(stderr, "[savedata-dlg] list (%s): %s, cancelled\n", type_name(g_dlg.type),
                n ? "no picker available" : "no saves");
        choose(-2);
        return;
    }
    fprintf(stderr, "[savedata-dlg] list (%s): %d entries shown\n", type_name(g_dlg.type), n);
    g_dlg.choosing = 1;
    g_dlg.status = ORBIS_COMMON_DIALOG_STATUS_RUNNING;
}

static void poll_choice(void) {
    if (!g_dlg.choosing) return;
    const int choice = ps4_gow3gpu_choice_poll();
    if (choice != -1) choose(choice);
}

static int32_t dialog_open(GuestContext *ctx, uint64_t param_addr) {
    const uint8_t *param = guest_bytes(ctx, param_addr, PARAM_SIZE);
    if (!param) return DLG_ERROR_ARG_NULL;
    pthread_mutex_lock(&g_mutex);
    // God of War III leaves its loading progress bar open: a new dialog ends it. Only a list
    // the player is still choosing from is busy.
    const bool open_progress = g_dlg.status == ORBIS_COMMON_DIALOG_STATUS_RUNNING &&
                               g_dlg.mode == ORBIS_SAVE_DATA_DIALOG_MODE_PROGRESS_BAR;
    if (g_dlg.status != ORBIS_COMMON_DIALOG_STATUS_INITIALIZED &&
        g_dlg.status != ORBIS_COMMON_DIALOG_STATUS_FINISHED && !open_progress) {
        pthread_mutex_unlock(&g_mutex);
        fprintf(stderr, "[savedata-dlg] open refused: a list is still open\n");
        return DLG_ERROR_INVALID_STATE;
    }
    memset(&g_dlg, 0, sizeof(g_dlg));
    g_dlg.mode = read_u32(param + PARAM_MODE);
    g_dlg.type = read_u32(param + PARAM_TYPE);
    g_dlg.user_data = read_u64(param + PARAM_USER_DATA);
    g_dlg.status = ORBIS_COMMON_DIALOG_STATUS_RUNNING;
    int32_t ret = 0;
    switch (g_dlg.mode) {
    case ORBIS_SAVE_DATA_DIALOG_MODE_LIST:
        open_list(ctx, param);
        break;
    case ORBIS_SAVE_DATA_DIALOG_MODE_PROGRESS_BAR:
        // The game's own work: runs until the game closes it.
        break;
    case ORBIS_SAVE_DATA_DIALOG_MODE_USER_MSG:
    case ORBIS_SAVE_DATA_DIALOG_MODE_SYSTEM_MSG:
    case ORBIS_SAVE_DATA_DIALOG_MODE_ERROR_CODE:
        finish(ORBIS_COMMON_DIALOG_RESULT_OK, DLG_BUTTON_OK);
        break;
    default:
        g_dlg.status = ORBIS_COMMON_DIALOG_STATUS_INITIALIZED;
        ret = DLG_ERROR_PARAM_INVALID;
        break;
    }
    if (g_dlg.mode != ORBIS_SAVE_DATA_DIALOG_MODE_LIST)
        fprintf(stderr, "[savedata-dlg] open: mode=%u type=%u -> %s\n", g_dlg.mode, g_dlg.type,
                ret ? "invalid mode" : g_dlg.status == ORBIS_COMMON_DIALOG_STATUS_RUNNING ? "running" : "OK");
    pthread_mutex_unlock(&g_mutex);
    return ret;
}

static int32_t dialog_result(GuestContext *ctx, uint64_t result_addr) {
    uint8_t *out = guest_bytes(ctx, result_addr, RESULT_SIZE);
    if (!out) return DLG_ERROR_ARG_NULL;
    pthread_mutex_lock(&g_mutex);
    if (g_dlg.status != ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
        pthread_mutex_unlock(&g_mutex);
        return DLG_ERROR_NOT_FINISHED;
    }
    memcpy(out + RESULT_MODE, &g_dlg.mode, 4);
    memcpy(out + RESULT_RESULT, &g_dlg.result, 4);
    memcpy(out + RESULT_BUTTON, &g_dlg.button, 4);
    // dirName and param point at the game's buffers: fill them, keep the pointers.
    const bool chosen = g_dlg.mode == ORBIS_SAVE_DATA_DIALOG_MODE_LIST &&
                        g_dlg.result == ORBIS_COMMON_DIALOG_RESULT_OK;
    uint8_t *dir = guest_bytes(ctx, read_u64(out + RESULT_DIR), DLG_DIRNAME_SIZE);
    if (dir) {
        memset(dir, 0, DLG_DIRNAME_SIZE);
        if (chosen) memcpy(dir, g_dlg.chosen, strnlen(g_dlg.chosen, DLG_DIRNAME_SIZE - 1));
    }
    uint8_t *param = guest_bytes(ctx, read_u64(out + RESULT_PARAM), sizeof(Ps4SaveDataParam));
    if (param && chosen && g_dlg.chosen[0]) {
        Ps4SaveDataParam p;
        if (ps4_savedata_read_dir_info(g_dlg.user_id, g_dlg.chosen, &p, NULL))
            memcpy(param, &p, sizeof(p));
    }
    memcpy(out + RESULT_USER_DATA, &g_dlg.user_data, 8);
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static int32_t dialog_status(void) {
    pthread_mutex_lock(&g_mutex);
    poll_choice();
    const OrbisCommonDialogStatus status = g_dlg.status;
    pthread_mutex_unlock(&g_mutex);
    return (int32_t)status;
}

static int32_t dialog_close(void) {
    pthread_mutex_lock(&g_mutex);
    if (g_dlg.status != ORBIS_COMMON_DIALOG_STATUS_RUNNING) {
        pthread_mutex_unlock(&g_mutex);
        return DLG_ERROR_NOT_RUNNING;
    }
    // A list closed by the game (not by the player) ends without a choice.
    if (g_dlg.choosing) finish(ORBIS_COMMON_DIALOG_RESULT_USER_CANCELED, DLG_BUTTON_INVALID);
    else finish(ORBIS_COMMON_DIALOG_RESULT_OK, g_dlg.button);
    fprintf(stderr, "[savedata-dlg] closed by the game (mode=%u)\n", g_dlg.mode);
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

// Shims
void shim_sceSaveDataDialogInitialize(GuestContext *ctx) {
    pthread_mutex_lock(&g_mutex);
    memset(&g_dlg, 0, sizeof(g_dlg));
    g_dlg.status = ORBIS_COMMON_DIALOG_STATUS_INITIALIZED;
    pthread_mutex_unlock(&g_mutex);
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_sceSaveDataDialogOpen(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)dialog_open(ctx, ctx->rdi);
    SHIM_RETURN();
}

void shim_sceSaveDataDialogUpdateStatus(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)dialog_status();
    SHIM_RETURN();
}

void shim_sceSaveDataDialogGetStatus(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)dialog_status();
    SHIM_RETURN();
}

void shim_sceSaveDataDialogGetResult(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)dialog_result(ctx, ctx->rdi);
    SHIM_RETURN();
}

void shim_sceSaveDataDialogClose(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)dialog_close();
    SHIM_RETURN();
}

void shim_sceSaveDataDialogIsReadyToDisplay(GuestContext *ctx) {
    ctx->rax = 1;
    SHIM_RETURN();
}

void shim_sceSaveDataDialogProgressBarSetValue(GuestContext *ctx) {
    pthread_mutex_lock(&g_mutex);
    const bool running = g_dlg.status == ORBIS_COMMON_DIALOG_STATUS_RUNNING &&
                         g_dlg.mode == ORBIS_SAVE_DATA_DIALOG_MODE_PROGRESS_BAR;
    pthread_mutex_unlock(&g_mutex);
    ctx->rax = (uint64_t)(int64_t)(running ? 0 : DLG_ERROR_NOT_RUNNING);
    SHIM_RETURN();
}

void shim_sceSaveDataDialogTerminate(GuestContext *ctx) {
    pthread_mutex_lock(&g_mutex);
    memset(&g_dlg, 0, sizeof(g_dlg));
    pthread_mutex_unlock(&g_mutex);
    ctx->rax = 0;
    SHIM_RETURN();
}
