#import <Cocoa/Cocoa.h>
#include "ps4_savedata_dialog.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_savedata_mutex = PTHREAD_MUTEX_INITIALIZER;
static OrbisCommonDialogStatus g_savedata_status = ORBIS_COMMON_DIALOG_STATUS_NONE;
static OrbisSaveDataDialogParam g_savedata_param = {0};
static OrbisSaveDataDialogResult g_savedata_result = {0};

int32_t sceSaveDataDialogInitialize(void) {
    pthread_mutex_lock(&g_savedata_mutex);
    g_savedata_status = ORBIS_COMMON_DIALOG_STATUS_INITIALIZED;
    memset(&g_savedata_result, 0, sizeof(g_savedata_result));
    memset(&g_savedata_param, 0, sizeof(g_savedata_param));
    pthread_mutex_unlock(&g_savedata_mutex);
    return 0;
}

int32_t sceSaveDataDialogOpen(const OrbisSaveDataDialogParam *param) {
    if (!param) {
        return (int32_t)0x80b8000d; // ARG_NULL
    }

    pthread_mutex_lock(&g_savedata_mutex);
    if (g_savedata_status != ORBIS_COMMON_DIALOG_STATUS_INITIALIZED &&
        g_savedata_status != ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
        pthread_mutex_unlock(&g_savedata_mutex);
        return (int32_t)0x80b80006; // INVALID_STATE
    }

    g_savedata_param = *param;
    memset(&g_savedata_result, 0, sizeof(g_savedata_result));
    g_savedata_result.mode = param->mode;
    g_savedata_status = ORBIS_COMMON_DIALOG_STATUS_RUNNING;
    pthread_mutex_unlock(&g_savedata_mutex);

    fprintf(stderr, "[savedata-dlg] sceSaveDataDialogOpen: mode=%d, dispType=%d\n",
            param->mode, param->dispType);
    return 0;
}

OrbisCommonDialogStatus sceSaveDataDialogGetStatus(void) {
    pthread_mutex_lock(&g_savedata_mutex);
    OrbisCommonDialogStatus st = g_savedata_status;
    pthread_mutex_unlock(&g_savedata_mutex);
    return st;
}

OrbisCommonDialogStatus sceSaveDataDialogUpdateStatus(void) {
    pthread_mutex_lock(&g_savedata_mutex);
    if (g_savedata_status != ORBIS_COMMON_DIALOG_STATUS_RUNNING) {
        OrbisCommonDialogStatus st = g_savedata_status;
        pthread_mutex_unlock(&g_savedata_mutex);
        return st;
    }

    // Headless or non-interactive fallback
    if (getenv("PS4_RECOMP_HEADLESS") != NULL || (!isatty(fileno(stdin)) && !isatty(fileno(stdout)))) {
        g_savedata_result.mode = g_savedata_param.mode;
        g_savedata_result.result = ORBIS_COMMON_DIALOG_RESULT_OK;
        g_savedata_result.buttonId = 1; // OK / YES
        g_savedata_status = ORBIS_COMMON_DIALOG_STATUS_FINISHED;
        OrbisCommonDialogStatus st = g_savedata_status;
        pthread_mutex_unlock(&g_savedata_mutex);
        return st;
    }

    // Progress bar mode does not need modal blocking
    if (g_savedata_param.mode == ORBIS_SAVE_DATA_DIALOG_MODE_PROGRESS_BAR) {
        g_savedata_result.mode = g_savedata_param.mode;
        g_savedata_result.result = ORBIS_COMMON_DIALOG_RESULT_OK;
        g_savedata_result.buttonId = 1;
        g_savedata_status = ORBIS_COMMON_DIALOG_STATUS_FINISHED;
        OrbisCommonDialogStatus st = g_savedata_status;
        pthread_mutex_unlock(&g_savedata_mutex);
        return st;
    }

    OrbisSaveDataDialogMode mode = g_savedata_param.mode;
    g_savedata_result.mode = mode;
    g_savedata_result.result = ORBIS_COMMON_DIALOG_RESULT_OK;
    g_savedata_result.buttonId = 1; // OK / YES
    g_savedata_status = ORBIS_COMMON_DIALOG_STATUS_FINISHED;

    static uint64_t s_dlg_update_count = 0;
    if ((++s_dlg_update_count % 10) == 1) {
        fprintf(stderr, "[savedata-dlg] sceSaveDataDialogUpdateStatus: mode=%d -> FINISHED (result=OK, button=1, total=%llu)\n",
                mode, (unsigned long long)s_dlg_update_count);
    }

    OrbisCommonDialogStatus final_st = g_savedata_status;
    pthread_mutex_unlock(&g_savedata_mutex);
    return final_st;
}

int32_t sceSaveDataDialogGetResult(OrbisSaveDataDialogResult *result) {
    if (!result) {
        return (int32_t)0x80b8000d; // ARG_NULL
    }

    pthread_mutex_lock(&g_savedata_mutex);
    if (g_savedata_status != ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
        pthread_mutex_unlock(&g_savedata_mutex);
        return (int32_t)0x80b80005; // NOT_FINISHED
    }

    *result = g_savedata_result;
    pthread_mutex_unlock(&g_savedata_mutex);
    return 0;
}

int32_t sceSaveDataDialogTerminate(void) {
    pthread_mutex_lock(&g_savedata_mutex);
    g_savedata_status = ORBIS_COMMON_DIALOG_STATUS_NONE;
    memset(&g_savedata_result, 0, sizeof(g_savedata_result));
    memset(&g_savedata_param, 0, sizeof(g_savedata_param));
    pthread_mutex_unlock(&g_savedata_mutex);
    return 0;
}

// Shims
void shim_sceSaveDataDialogInitialize(GuestContext *ctx) {
    ctx->rax = (uint64_t)sceSaveDataDialogInitialize();
    SHIM_RETURN();
}

void shim_sceSaveDataDialogOpen(GuestContext *ctx) {
    const OrbisSaveDataDialogParam *param = ctx->rdi ? (const OrbisSaveDataDialogParam *)(ctx->mem_base + ctx->rdi) : NULL;
    if (param) {
        fprintf(stderr, "[savedata-dlg] shim_sceSaveDataDialogOpen: caller RIP=0x%llx, mode=%d, dispType=%d\n",
                (unsigned long long)ctx->rip, param->mode, param->dispType);
    }
    ctx->rax = (uint64_t)sceSaveDataDialogOpen(param);
    SHIM_RETURN();
}

void shim_sceSaveDataDialogUpdateStatus(GuestContext *ctx) {
    ctx->rax = (uint64_t)sceSaveDataDialogUpdateStatus();
    SHIM_RETURN();
}

void shim_sceSaveDataDialogGetStatus(GuestContext *ctx) {
    ctx->rax = (uint64_t)sceSaveDataDialogGetStatus();
    SHIM_RETURN();
}

void shim_sceSaveDataDialogGetResult(GuestContext *ctx) {
    ctx->rax = (uint64_t)sceSaveDataDialogGetResult(
        ctx->rdi ? (OrbisSaveDataDialogResult *)(ctx->mem_base + ctx->rdi) : NULL
    );
    SHIM_RETURN();
}

void shim_sceSaveDataDialogTerminate(GuestContext *ctx) {
    ctx->rax = (uint64_t)sceSaveDataDialogTerminate();
    SHIM_RETURN();
}
