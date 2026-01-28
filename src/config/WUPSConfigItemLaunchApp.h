 #pragma once
#include <wups.h>
#include <stdint.h>
 #include "../homebrew/scanner.h"
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 typedef struct ConfigItemLaunchApp {
     const char *identifier;
     WUPSConfigItemHandle handle;
     HomebrewAppList app_list;
     int32_t current_index;
    char *apps_root;
    char *info_text;
    uint64_t info_until_ticks;
    int show_only_info;
 } ConfigItemLaunchApp;
 
 typedef void (*LaunchAppValueChangedCallback)(ConfigItemLaunchApp *, int32_t index);
 
 WUPSConfigAPIStatus
 WUPSConfigItemLaunchApp_Create(const char *identifier,
                                const char *displayName,
                                const char *apps_root,
                                LaunchAppValueChangedCallback callback,
                                WUPSConfigItemHandle *outHandle);
 
 WUPSConfigAPIStatus
 WUPSConfigItemLaunchApp_AddToCategory(WUPSConfigCategoryHandle cat,
                                       const char *identifier,
                                       const char *displayName,
                                       const char *apps_root,
                                       LaunchAppValueChangedCallback callback);
 
 #ifdef __cplusplus
 }
 #endif
