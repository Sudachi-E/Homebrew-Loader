 #pragma once
 #include <stddef.h>
 #include <stdbool.h>
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 typedef struct HomebrewApp {
     char *name;
     char *path;
     bool is_wuhb;
 } HomebrewApp;
 
 typedef struct HomebrewAppList {
     HomebrewApp *items;
     size_t count;
 } HomebrewAppList;
 
 bool hb_scan_apps(const char *root_path, HomebrewAppList *out_list);
 
 void hb_free_app_list(HomebrewAppList *list);
 
 #ifdef __cplusplus
 }
 #endif
