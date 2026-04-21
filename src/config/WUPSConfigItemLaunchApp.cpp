 #include "WUPSConfigItemLaunchApp.h"
 #include "../utils/logger.h"
 #include "../homebrew/scanner.h"
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
#include <wups.h>
#include <sys/stat.h>
#include <rpxloader/rpxloader.h>
#include <coreinit/time.h>
 
extern "C" WUPSConfigCategoryHandle gHomebrewRootCategory;
extern "C" void RegisterQuickFavorite(const char *identifier, const char *displayName, const char *path);
extern "C" void RemoveQuickFavoriteByPath(const char *path);
static const char *normalize_to_sd_relative(const char *path) {
    if (!path) return nullptr;
    if (strncmp(path, "sd:/", 4) == 0) {
        return path + 4;
    }
    const char *vol = strstr(path, "/vol/external01/");
    if (strncmp(path, "fs:/", 4) == 0 && vol) {
        return vol + strlen("/vol/external01/");
    }
    return path;
}

static void make_sanitized_identifier(char *dst, size_t dst_len, const char *name, int counter) {
    const char *prefix = "quick_";
    size_t plen = strlen(prefix);
    size_t nlen = name ? strlen(name) : 0;
    size_t pos  = 0;
    if (dst_len == 0) return;
    if (plen >= dst_len) plen = dst_len - 1;
    memcpy(dst, prefix, plen);
    pos += plen;
    if (pos < dst_len - 1) {
        int written = snprintf(dst + pos, dst_len - pos, "%d_", counter);
        if (written < 0) written = 0;
        pos += (size_t)written;
    }
    for (size_t i = 0; i < nlen && pos < dst_len - 1; i++) {
        char c = name[i];
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        dst[pos++] = c;
    }
    dst[pos] = 0;
}

 static int32_t getCurrentValueDisplay(void *context, char *out_buf, int32_t out_size) {
     auto *item = (ConfigItemLaunchApp *) context;
     if (!item || item->app_list.count == 0) {
         snprintf(out_buf, out_size, "No apps found");
         return 0;
     }
     snprintf(out_buf, out_size, "\u25CB");
     return 0;
 }
 
 static void onCloseCallback(void *context) {
     auto *item = (ConfigItemLaunchApp *) context;
     if (!item) return;
 }
 
 static void onInput(void *context, WUPSConfigSimplePadData input) {
     auto *item = (ConfigItemLaunchApp *) context;
    if (!item) return;
    if (item->app_list.count == 0) return;
     if (input.buttons_d & WUPS_CONFIG_BUTTON_LEFT) {
         item->current_index--;
         if (item->current_index < 0) item->current_index = (int32_t) item->app_list.count - 1;
     } else if (input.buttons_d & WUPS_CONFIG_BUTTON_RIGHT) {
         item->current_index++;
         if (item->current_index >= (int32_t) item->app_list.count) item->current_index = 0;
    } else if (input.buttons_d & WUPS_CONFIG_BUTTON_Y) {
        if (item->identifier && strcmp(item->identifier, "launchApp") == 0) {
            DEBUG_FUNCTION_LINE_INFO("Y pressed on Launch homebrew");
            const HomebrewApp &app = item->app_list.items[item->current_index];
            const char *name = app.name ? app.name : app.path;
            if (gHomebrewRootCategory.handle != NULL) {
                char identifier[128];
                static int s_quick_counter = 1;
                make_sanitized_identifier(identifier, sizeof(identifier), name, s_quick_counter++);
                size_t dn_len = strlen(name) + 8;
                char *displayName = (char *)malloc(dn_len + 1);
                if (displayName) {
                    memcpy(displayName, "Quick: ", 7);
                    memcpy(displayName + 7, name, dn_len - 7);
                    displayName[dn_len] = 0;
                }
                WUPSConfigAPIStatus st = WUPSConfigItemLaunchApp_AddToCategory(
                        gHomebrewRootCategory,
                        identifier,
                        displayName ? displayName : name,
                        app.path,
                        (LaunchAppValueChangedCallback)NULL);
                if (st != WUPSCONFIG_API_RESULT_SUCCESS) {
                    DEBUG_FUNCTION_LINE_ERR("Quick add failed (%d) for %s", (int)st, name);
                } else {
                    DEBUG_FUNCTION_LINE_INFO("Quick add created for %s", name);
                }
                RegisterQuickFavorite(identifier, displayName ? displayName : name, app.path);
                const char *msg = "Close and reopen the config menu for it to show.";
                size_t ml = strlen(msg);
                free(item->info_text);
                item->info_text = (char *)malloc(ml + 1);
                if (item->info_text) {
                    memcpy(item->info_text, msg, ml + 1);
                }
                item->show_only_info   = 1;
                item->info_until_ticks = (uint64_t) OSGetTime() + (uint64_t) OSMillisecondsToTicks(5000);
                free(displayName);
            } else {
                DEBUG_FUNCTION_LINE_ERR("Quick add unavailable: invalid root category");
            }
        } else {
            DEBUG_FUNCTION_LINE_INFO("Y pressed on Quick item - ignored");
        }
    } else if (input.buttons_d & WUPS_CONFIG_BUTTON_X) {
        if (item->app_list.count == 0) return;
        const HomebrewApp &app = item->app_list.items[item->current_index];
        if (!(item->identifier && strcmp(item->identifier, "launchApp") == 0)) {
            DEBUG_FUNCTION_LINE_INFO("X pressed on Quick item - remove quick for %s", app.path);
            RemoveQuickFavoriteByPath(app.path);
            const char *msg2 = "Close and reopen the config menu to update.";
            size_t m2 = strlen(msg2);
            free(item->info_text);
            item->info_text = (char *)malloc(m2 + 1);
            if (item->info_text) {
                memcpy(item->info_text, msg2, m2 + 1);
            }
            item->show_only_info   = 1;
            item->info_until_ticks = (uint64_t) OSGetTime() + (uint64_t) OSMillisecondsToTicks(5000);
        }
    } else if (input.buttons_d & WUPS_CONFIG_BUTTON_A) {
        if (item->app_list.count == 0) return;
        DEBUG_FUNCTION_LINE_INFO("Launching homebrew: %s (%s)", item->app_list.items[item->current_index].name, item->app_list.items[item->current_index].path);
        const HomebrewApp &app = item->app_list.items[item->current_index];
        RPXLoaderStatus initRes = RPXLoader_InitLibrary();
        if (initRes != RPX_LOADER_RESULT_SUCCESS) {
            DEBUG_FUNCTION_LINE_ERR("RPXLoader_InitLibrary failed: %s", RPXLoader_GetStatusStr(initRes));
            return;
        }
        const char *p = normalize_to_sd_relative(app.path);
        RPXLoaderStatus res = RPXLoader_LaunchHomebrew(p);
        if (res != RPX_LOADER_RESULT_SUCCESS) {
            DEBUG_FUNCTION_LINE_ERR("Failed to launch: %s (err=%s)", app.path, RPXLoader_GetStatusStr(res));
        }
        RPXLoader_DeInitLibrary();
     }
 }
 
 static int32_t getCurrentValueSelectedDisplay(void *context, char *out_buf, int32_t out_size) {
     auto *item = (ConfigItemLaunchApp *) context;
     if (!item || item->app_list.count == 0) {
        snprintf(out_buf, out_size, "No apps found");
         return 0;
     }
     const HomebrewApp &app = item->app_list.items[item->current_index];
    if (item->show_only_info) {
        if ((uint64_t) OSGetTime() < item->info_until_ticks) {
            if (item->info_text && item->info_text[0]) {
                snprintf(out_buf, out_size, "%s", item->info_text);
            } else {
                snprintf(out_buf, out_size, " ");
            }
            return 0;
        } else {
            item->show_only_info = 0;
            free(item->info_text);
            item->info_text = nullptr;
        }
    }
    if (item->identifier && strcmp(item->identifier, "launchApp") == 0) {
        if (item->info_text && item->info_text[0]) {
            snprintf(out_buf, out_size, "Launch: %s \uE000  Y: Quick  - %s", app.name, item->info_text);
        } else {
            snprintf(out_buf, out_size, "Launch: %s \uE000  Y: Quick", app.name);
        }
    } else {
        if (item->info_text && item->info_text[0]) {
            snprintf(out_buf, out_size, "Launch: %s \uE000  X: Remove  - %s", app.name, item->info_text);
        } else {
            snprintf(out_buf, out_size, "Launch: %s \uE000  X: Remove", app.name);
        }
    }
     return 0;
 }
 
 static void restoreDefault(void *context) {
     auto *item = (ConfigItemLaunchApp *) context;
     if (!item) return;
     item->current_index = 0;
 }
 
 static void onSelected(void *context, bool isSelected) {
     auto *item = (ConfigItemLaunchApp *) context;
     if (!item) return;
 }
 
 static void Cleanup(ConfigItemLaunchApp *item) {
     if (!item) return;
     hb_free_app_list(&item->app_list);
    free(item->apps_root);
    free(item->info_text);
     free((void *) item->identifier);
     free(item);
 }
 
 static void onDelete(void *context) {
     Cleanup((ConfigItemLaunchApp *) context);
 }
 

 extern "C" WUPSConfigAPIStatus
 WUPSConfigItemLaunchApp_Create(const char *identifier,
                                const char *displayName,
                                const char *apps_root,
                                LaunchAppValueChangedCallback callback,
                                WUPSConfigItemHandle *outHandle) {
     if (outHandle == nullptr) {
         return WUPSCONFIG_API_RESULT_INVALID_ARGUMENT;
     }
     auto *item = (ConfigItemLaunchApp *) malloc(sizeof(ConfigItemLaunchApp));
     if (item == nullptr) {
         return WUPSCONFIG_API_RESULT_OUT_OF_MEMORY;
     }
 
     if (identifier != nullptr) {
         item->identifier = strdup(identifier);
     } else {
         item->identifier = nullptr;
     }
     item->current_index = 0;
     item->app_list.items = nullptr;
     item->app_list.count = 0;
    item->apps_root      = apps_root ? strdup(apps_root) : strdup("sd:/wiiu/apps");
    item->info_text      = nullptr;
    item->info_until_ticks = 0;
    item->show_only_info   = 0;
 
    bool is_rpx = false;
    bool is_wuhb = false;
    if (item->apps_root) {
        size_t len = strlen(item->apps_root);
        if (len >= 4 && strncmp(item->apps_root + len - 4, ".rpx", 4) == 0) is_rpx = true;
        if (len >= 5 && strncmp(item->apps_root + len - 5, ".wuhb", 5) == 0) is_wuhb = true;
    }
    if (is_rpx || is_wuhb) {
        item->app_list.items = (HomebrewApp *)malloc(sizeof(HomebrewApp));
        if (item->app_list.items) {
            item->app_list.items[0].path = strdup(item->apps_root);
            item->app_list.items[0].is_wuhb = is_wuhb;
            const char *bn = item->apps_root;
            const char *slash = strrchr(item->apps_root, '/');
            if (slash && *(slash + 1)) bn = slash + 1;
            item->app_list.items[0].name = strdup(bn);
            item->app_list.count = 1;
        }
    } else {
        hb_scan_apps(item->apps_root, &item->app_list);
        if (item->app_list.count == 0 && strncmp(item->apps_root, "sd:/", 4) == 0) {
            hb_scan_apps("fs:/vol/external01/wiiu/apps", &item->app_list);
        }
    }
 
     WUPSConfigAPIItemCallbacksV2 callbacks = {
             .getCurrentValueDisplay         = &getCurrentValueDisplay,
             .getCurrentValueSelectedDisplay = &getCurrentValueSelectedDisplay,
             .onSelected                     = &onSelected,
             .restoreDefault                 = &restoreDefault,
             .isMovementAllowed              = nullptr,
             .onCloseCallback                = &onCloseCallback,
             .onInput                        = &onInput,
             .onInputEx                      = nullptr,
             .onDelete                       = &onDelete};
 
     WUPSConfigAPIItemOptionsV2 options = {
             .displayName = displayName,
             .context     = item,
             .callbacks   = callbacks};
 
     WUPSConfigAPIStatus err;
     if ((err = WUPSConfigAPI_Item_Create(options, &item->handle)) != WUPSCONFIG_API_RESULT_SUCCESS) {
         Cleanup(item);
         return err;
     }
 
     *outHandle = item->handle;
     return WUPSCONFIG_API_RESULT_SUCCESS;
 }
 
 extern "C" WUPSConfigAPIStatus
 WUPSConfigItemLaunchApp_AddToCategory(WUPSConfigCategoryHandle cat,
                                       const char *identifier,
                                       const char *displayName,
                                       const char *apps_root,
                                       LaunchAppValueChangedCallback callback) {
     WUPSConfigItemHandle itemHandle;
     WUPSConfigAPIStatus res;
    if ((res = WUPSConfigItemLaunchApp_Create(identifier,
                                               displayName,
                                               apps_root,
                                               callback,
                                               &itemHandle)) != WUPSCONFIG_API_RESULT_SUCCESS) {
         return res;
     }
 
     if ((res = WUPSConfigAPI_Category_AddItem(cat, itemHandle)) != WUPSCONFIG_API_RESULT_SUCCESS) {
         WUPSConfigAPI_Item_Destroy(itemHandle);
         return res;
     }
     return WUPSCONFIG_API_RESULT_SUCCESS;
 }
