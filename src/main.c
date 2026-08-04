#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <wups.h>
#include <wups/storage.h>
#include <wups/function_patching.h>
#include <wups/button_combo/api.h>
#include <wups/config_api.h>
#include <wups/config/WUPSConfigItemButtonCombo.h>
#include "menu/cache.h"
#include "menu/menu.h"
#include <gx2/surface.h>
#include <gx2/display.h>
#include <notifications/notifications.h>
#include <coreinit/thread.h>
#include <vpad/input.h>

WUPS_PLUGIN_NAME("Homebrew Loader");
WUPS_PLUGIN_DESCRIPTION("Browse and load homebrew from sd:/wiiu/apps");
WUPS_PLUGIN_VERSION("v1.4");
WUPS_PLUGIN_AUTHOR("SudoTronics");
WUPS_PLUGIN_LICENSE("GPL");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("homebrew_loader");

typedef struct {
    void *buffer;
    uint32_t buffer_size;
    int32_t mode;
    GX2SurfaceFormat surface_format;
    GX2BufferingMode buffering_mode;
} StoredBuffer;

static bool s_notifModuleLoaded = false;

static StoredBuffer gStoredTVBuffer;
static StoredBuffer gStoredDRCBuffer;

StoredBuffer *Menu_GetStoredTVBuffer(void) { return &gStoredTVBuffer; }
StoredBuffer *Menu_GetStoredDRCBuffer(void) { return &gStoredDRCBuffer; }
bool Menu_NotificationModuleLoaded(void) { return s_notifModuleLoaded; }

DECL_FUNCTION(void, GX2SetTVBuffer_hook, void *buffer, uint32_t buffer_size,
              int32_t tv_render_mode, GX2SurfaceFormat format,
              GX2BufferingMode buffering_mode) {
    gStoredTVBuffer.buffer = buffer;
    gStoredTVBuffer.buffer_size = buffer_size;
    gStoredTVBuffer.mode = tv_render_mode;
    gStoredTVBuffer.surface_format = format;
    gStoredTVBuffer.buffering_mode = buffering_mode;
    real_GX2SetTVBuffer_hook(buffer, buffer_size, tv_render_mode, format, buffering_mode);
}

WUPS_MUST_REPLACE(GX2SetTVBuffer_hook, WUPS_LOADER_LIBRARY_GX2, GX2SetTVBuffer);

DECL_FUNCTION(void, GX2SetDRCBuffer_hook, void *buffer, uint32_t buffer_size,
              uint32_t drc_mode, GX2SurfaceFormat surface_format,
              GX2BufferingMode buffering_mode) {
    gStoredDRCBuffer.buffer = buffer;
    gStoredDRCBuffer.buffer_size = buffer_size;
    gStoredDRCBuffer.mode = drc_mode;
    gStoredDRCBuffer.surface_format = surface_format;
    gStoredDRCBuffer.buffering_mode = buffering_mode;
    real_GX2SetDRCBuffer_hook(buffer, buffer_size, drc_mode, surface_format, buffering_mode);
}
WUPS_MUST_REPLACE(GX2SetDRCBuffer_hook, WUPS_LOADER_LIBRARY_GX2, GX2SetDRCBuffer);

typedef struct QuickFav {
    char *identifier;
    char *displayName;
    char *path;
} QuickFav;

#define MAX_QUICK_FAVS 32
QuickFav s_quickFavs[MAX_QUICK_FAVS];
int s_quickFavCount = 0;
static const char *QUICK_FAV_COUNT_KEY = "quickFavCount";

static void storeBlocks(const char *prefix, const char *label, int index, const char *s) {
    if (!s) return;
    char key[128];
    snprintf(key, sizeof(key), "%s_%d_%s_len", prefix, index, label);
    WUPSStorageAPI_StoreU32(NULL, key, (uint32_t)strlen(s));
    size_t len = strlen(s);
    size_t groups = (len + 3) / 4;
    for (size_t g = 0; g < groups; g++) {
        uint32_t v = 0;
        size_t base = g * 4;
        for (size_t b = 0; b < 4; b++) {
            size_t pos = base + b;
            uint8_t ch = (pos < len) ? (uint8_t)s[pos] : 0;
            v |= ((uint32_t)ch) << (8 * b);
        }
        snprintf(key, sizeof(key), "%s_%d_%s_blk_%u", prefix, index, label, (unsigned)g);
        WUPSStorageAPI_StoreU32(NULL, key, v);
    }
}

static char *loadBlocks(const char *prefix, const char *label, int index) {
    char key[128];
    uint32_t lenU32 = 0;
    snprintf(key, sizeof(key), "%s_%d_%s_len", prefix, index, label);
    if (WUPSStorageAPI_GetU32(NULL, key, &lenU32) != WUPS_STORAGE_ERROR_SUCCESS || lenU32 == 0) {
        return NULL;
    }
    size_t len = (size_t)lenU32;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t groups = (len + 3) / 4;
    size_t written = 0;
    for (size_t g = 0; g < groups; g++) {
        uint32_t v = 0;
        snprintf(key, sizeof(key), "%s_%d_%s_blk_%u", prefix, index, label, (unsigned)g);
        if (WUPSStorageAPI_GetU32(NULL, key, &v) != WUPS_STORAGE_ERROR_SUCCESS) {
            free(out);
            return NULL;
        }
        for (size_t b = 0; b < 4 && written < len; b++) {
            uint8_t ch = (uint8_t)((v >> (8 * b)) & 0xFF);
            out[written++] = (char)ch;
        }
    }
    out[len] = 0;
    return out;
}

static void SaveQuickFavorites(void) {
    WUPSStorageAPI_StoreU32(NULL, QUICK_FAV_COUNT_KEY, (uint32_t)s_quickFavCount);
    for (int i = 0; i < s_quickFavCount; i++) {
        storeBlocks("quickFav", "id", i, s_quickFavs[i].identifier);
        storeBlocks("quickFav", "dn", i, s_quickFavs[i].displayName);
        storeBlocks("quickFav", "path", i, s_quickFavs[i].path);
    }
    WUPSStorageAPI_SaveStorage(false);
}

static void FreeQuickFavorites(void) {
    for (int i = 0; i < s_quickFavCount; i++) {
        free(s_quickFavs[i].identifier);
        free(s_quickFavs[i].displayName);
        free(s_quickFavs[i].path);
        s_quickFavs[i].identifier = NULL;
        s_quickFavs[i].displayName = NULL;
        s_quickFavs[i].path = NULL;
    }
    s_quickFavCount = 0;
}

static void LoadQuickFavorites(void) {
    FreeQuickFavorites();
    uint32_t countU32 = 0;
    if (WUPSStorageAPI_GetU32(NULL, QUICK_FAV_COUNT_KEY, &countU32) != WUPS_STORAGE_ERROR_SUCCESS || countU32 == 0) {
        return;
    }
    int count = (int)countU32;
    if (count > MAX_QUICK_FAVS) count = MAX_QUICK_FAVS;
    for (int i = 0; i < count; i++) {
        char *id = loadBlocks("quickFav", "id", i);
        char *dn = loadBlocks("quickFav", "dn", i);
        char *path = loadBlocks("quickFav", "path", i);
        if (id && dn && path) {
            s_quickFavs[s_quickFavCount].identifier = id;
            s_quickFavs[s_quickFavCount].displayName = dn;
            s_quickFavs[s_quickFavCount].path = path;
            s_quickFavCount++;
        } else {
            free(id);
            free(dn);
            free(path);
        }
    }
}

bool IsPathFavorited(const char *path) {
    if (!path) return false;
    for (int i = 0; i < s_quickFavCount; i++) {
        if (s_quickFavs[i].path && strcmp(s_quickFavs[i].path, path) == 0) return true;
    }
    return false;
}

void RegisterQuickFavorite(const char *identifier, const char *displayName, const char *path) {
    if (!identifier || !displayName || !path) return;
    if (s_quickFavCount >= MAX_QUICK_FAVS) return;
    for (int i = 0; i < s_quickFavCount; i++) {
        if (strcmp(s_quickFavs[i].path, path) == 0) return;
    }
    s_quickFavs[s_quickFavCount].identifier  = strdup(identifier);
    s_quickFavs[s_quickFavCount].displayName = strdup(displayName);
    s_quickFavs[s_quickFavCount].path        = strdup(path);
    if (!s_quickFavs[s_quickFavCount].identifier ||
        !s_quickFavs[s_quickFavCount].displayName ||
        !s_quickFavs[s_quickFavCount].path) {
        free(s_quickFavs[s_quickFavCount].identifier);
        free(s_quickFavs[s_quickFavCount].displayName);
        free(s_quickFavs[s_quickFavCount].path);
        s_quickFavs[s_quickFavCount].identifier = NULL;
        s_quickFavs[s_quickFavCount].displayName = NULL;
        s_quickFavs[s_quickFavCount].path = NULL;
        return;
    }
    s_quickFavCount++;
    SaveQuickFavorites();
}

void RemoveQuickFavoriteByPath(const char *path) {
    if (!path) return;
    for (int i = 0; i < s_quickFavCount; i++) {
        if (s_quickFavs[i].path && strcmp(s_quickFavs[i].path, path) == 0) {
            free(s_quickFavs[i].identifier);
            free(s_quickFavs[i].displayName);
            free(s_quickFavs[i].path);
            for (int j = i; j < s_quickFavCount - 1; j++) {
                s_quickFavs[j] = s_quickFavs[j + 1];
            }
            s_quickFavs[s_quickFavCount - 1].identifier  = NULL;
            s_quickFavs[s_quickFavCount - 1].displayName = NULL;
            s_quickFavs[s_quickFavCount - 1].path        = NULL;
            s_quickFavCount--;
            SaveQuickFavorites();
            break;
        }
    }
}

#define OPEN_COMBO_DEFAULT (VPAD_BUTTON_L | VPAD_BUTTON_R | VPAD_BUTTON_DOWN)
#define OPEN_COMBO_STORAGE_KEY "openCombo"

static WUPSButtonCombo_ComboHandle g_comboHandle;
static uint32_t g_currentCombo = OPEN_COMBO_DEFAULT;
static bool s_menuOpen = false;
static OSThread *g_menuThread = NULL;

DECL_FUNCTION(int32_t, VPADRead_hook, int32_t chan, VPADStatus *buffer, uint32_t buffer_size, VPADReadError *error) {
    if (s_menuOpen && OSGetCurrentThread() != g_menuThread) {
        if (error) *error = VPAD_READ_NO_SAMPLES;
        return 0;
    }
    return real_VPADRead_hook(chan, buffer, buffer_size, error);
}

WUPS_MUST_REPLACE(VPADRead_hook, WUPS_LOADER_LIBRARY_VPAD, VPADRead);

static void openMenu(void) {
    if (s_menuOpen) return;
    s_menuOpen = true;
    g_menuThread = OSGetCurrentThread();
    Menu_Open();
    g_menuThread = NULL;
    s_menuOpen = false;
}

static void comboCallback(WUPSButtonCombo_ControllerTypes triggeredBy,
                           WUPSButtonCombo_ComboHandle handle,
                           void *context) {
    (void)triggeredBy;
    (void)handle;
    (void)context;
    openMenu();
}

static void ConfigComboValueChanged(ConfigItemButtonCombo *item, uint32_t newCombo) {
    (void)item;
    g_currentCombo = newCombo;
    WUPSButtonComboAPI_UpdateButtonCombo(g_comboHandle, (WUPSButtonCombo_Buttons)newCombo, NULL);
}

static WUPSConfigAPICallbackStatus ConfigMenuOpened(WUPSConfigCategoryHandle root) {
    WUPSConfigItemButtonCombo_AddToCategory(root,
        OPEN_COMBO_STORAGE_KEY, "Open Menu Combo",
        (WUPSButtonCombo_Buttons)g_currentCombo, g_comboHandle,
        ConfigComboValueChanged);
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

static void ConfigMenuClosed(void) {}

INITIALIZE_PLUGIN() {
    s_notifModuleLoaded = NotificationModule_InitLibrary() == NOTIFICATION_MODULE_RESULT_SUCCESS;

    WUPSConfigAPIOptionsV1 configOpts = { .name = "Homebrew Loader" };
    WUPSConfigAPI_Init(configOpts, ConfigMenuOpened, ConfigMenuClosed);

    uint32_t saved = 0;
    if (WUPSStorageAPI_GetU32(NULL, OPEN_COMBO_STORAGE_KEY, &saved) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (saved != 0) {
            g_currentCombo = saved;
        }
    }

    LoadQuickFavorites();
    Menu_Init();

    WUPSButtonCombo_ComboStatus status;
    WUPSButtonComboAPI_AddButtonComboPressDownObserver(
        "Open Menu",
        (WUPSButtonCombo_Buttons)g_currentCombo,
        comboCallback,
        NULL,
        &g_comboHandle,
        &status
    );
}

DEINITIALIZE_PLUGIN() {
    WUPSButtonComboAPI_RemoveButtonCombo(g_comboHandle);
    NotificationModule_DeInitLibrary();
}

ON_APPLICATION_START() {
}

ON_APPLICATION_ENDS() {
}

ON_APPLICATION_REQUESTS_EXIT() {
}

ON_ACQUIRED_FOREGROUND() {
}

ON_RELEASE_FOREGROUND() {
}
