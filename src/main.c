 #include "utils/logger.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
 #include <wups.h>
 #include <wups/storage.h>
 #include <wups/config_api.h>
 #include "config/WUPSConfigItemLaunchApp.h"
 
 WUPS_PLUGIN_NAME("Homebrew Loader");
 WUPS_PLUGIN_DESCRIPTION("Browse and load homebrew from sd:/wiiu/apps");
 WUPS_PLUGIN_VERSION("v0.1");
 WUPS_PLUGIN_AUTHOR("SudoTronics");
 WUPS_PLUGIN_LICENSE("GPL");
 
 WUPS_USE_WUT_DEVOPTAB();
 WUPS_USE_STORAGE("homebrew_loader");
WUPSConfigCategoryHandle gHomebrewRootCategory;
typedef struct QuickFav {
    char *identifier;
    char *displayName;
    char *path;
} QuickFav;
static QuickFav s_quickFavs[32];
static int s_quickFavCount = 0;
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
    if (count > (int)(sizeof(s_quickFavs) / sizeof(s_quickFavs[0]))) count = (int)(sizeof(s_quickFavs) / sizeof(s_quickFavs[0]));
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
void RegisterQuickFavorite(const char *identifier, const char *displayName, const char *path) {
    if (!identifier || !displayName || !path) return;
    if (s_quickFavCount >= (int)(sizeof(s_quickFavs) / sizeof(s_quickFavs[0]))) return;
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
    DEBUG_FUNCTION_LINE_INFO("Quick favorite registered: %s", path);
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
            DEBUG_FUNCTION_LINE_INFO("Quick favorite removed: %s", path);
            SaveQuickFavorites();
            break;
        }
    }
}

 static WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle root) {
   gHomebrewRootCategory = root;
   const char *apps_root = "sd:/wiiu/apps";
     if (WUPSConfigItemLaunchApp_AddToCategory(root,
                                               "launchApp",
                                               "Launch homebrew",
                                               apps_root,
                                               (LaunchAppValueChangedCallback) NULL) != WUPSCONFIG_API_RESULT_SUCCESS) {
         DEBUG_FUNCTION_LINE_ERR("Failed to add 'Launch homebrew' item");
     }
     for (int i = 0; i < s_quickFavCount; i++) {
         if (s_quickFavs[i].identifier && s_quickFavs[i].displayName && s_quickFavs[i].path) {
             if (WUPSConfigItemLaunchApp_AddToCategory(root,
                                                       s_quickFavs[i].identifier,
                                                       s_quickFavs[i].displayName,
                                                       s_quickFavs[i].path,
                                                       (LaunchAppValueChangedCallback) NULL) != WUPSCONFIG_API_RESULT_SUCCESS) {
                 DEBUG_FUNCTION_LINE_ERR("Failed to add quick item: %s", s_quickFavs[i].displayName);
             } else {
                 DEBUG_FUNCTION_LINE_INFO("Quick item added: %s", s_quickFavs[i].displayName);
             }
         }
     }
     return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
 }
 
 static void ConfigMenuClosedCallback() {
    gHomebrewRootCategory.handle = NULL;
    WUPSStorageAPI_SaveStorage(false);
 }
 
 INITIALIZE_PLUGIN() {
     initLogging();
     DEBUG_FUNCTION_LINE_INFO("INITIALIZE_PLUGIN of HomebrewLoader");
 
     WUPSConfigAPIOptionsV1 configOptions = {.name = "Homebrew Loader"};
     if (WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) != WUPSCONFIG_API_RESULT_SUCCESS) {
         DEBUG_FUNCTION_LINE_ERR("Failed to init config api");
     }
    LoadQuickFavorites();
 }
 
 DEINITIALIZE_PLUGIN() {
     DEBUG_FUNCTION_LINE_INFO("DEINITIALIZE_PLUGIN of HomebrewLoader");
 }
 
 ON_APPLICATION_START() {
     initLogging();
     DEBUG_FUNCTION_LINE_INFO("ON_APPLICATION_START of HomebrewLoader");
 }
 
 ON_APPLICATION_ENDS() {
     deinitLogging();
 }
 
 ON_APPLICATION_REQUESTS_EXIT() {
     DEBUG_FUNCTION_LINE_INFO("ON_APPLICATION_REQUESTS_EXIT of HomebrewLoader");
 }
