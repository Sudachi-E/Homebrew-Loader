 #include "scanner.h"
 #include "../utils/logger.h"
 #include <dirent.h>
 #include <sys/stat.h>
 #include <stdlib.h>
 #include <string.h>
 #include <stdio.h>
 
 static bool ends_with(const char *str, const char *suffix) {
     if (!str || !suffix) return false;
     size_t lenstr = strlen(str);
     size_t lensuf = strlen(suffix);
     if (lensuf > lenstr) return false;
     return strcasecmp(str + lenstr - lensuf, suffix) == 0;
 }
 
 static char *dupstr(const char *s) {
     if (!s) return NULL;
     size_t n = strlen(s) + 1;
     char *d  = (char *) malloc(n);
     if (d) memcpy(d, s, n);
     return d;
 }
 
static bool read_meta_name(const char *dir_path, char **out_name) {
    size_t dlen = strlen(dir_path);
    const char *sfx = "/meta.xml";
    size_t plen = dlen + strlen(sfx) + 1;
    char *path = (char *) malloc(plen);
    if (!path) return false;
    memcpy(path, dir_path, dlen);
    memcpy(path + dlen, sfx, strlen(sfx) + 1);
    FILE *f = fopen(path, "rb");
     if (!f) return false;
     char buf[2048];
     size_t r = fread(buf, 1, sizeof(buf) - 1, f);
     fclose(f);
    free(path);
     if (r == 0) return false;
     buf[r] = 0;
     const char *start = strstr(buf, "<name>");
     const char *end   = strstr(buf, "</name>");
     if (start && end && end > start) {
         start += 6;
         size_t n = (size_t) (end - start);
         char *name = (char *) malloc(n + 1);
         if (!name) return false;
         memcpy(name, start, n);
         name[n] = 0;
         *out_name = name;
         return true;
     }
     return false;
 }
 
static void scan_dir(const char *dir_path, HomebrewApp **items, size_t *count, size_t *cap) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t dlen = strlen(dir_path);
        size_t nlen = strlen(ent->d_name);
        size_t plen = dlen + 1 + nlen + 1;
        char *full  = (char *) malloc(plen);
        if (!full) continue;
        memcpy(full, dir_path, dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1, ent->d_name, nlen + 1);
        struct stat st;
        if (stat(full, &st) != 0) {
            free(full);
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            if (!(ends_with(ent->d_name, ".wuhb") || ends_with(ent->d_name, ".rpx"))) {
                free(full);
                continue;
            }
            if (*count == *cap) {
                *cap *= 2;
                HomebrewApp *newItems = (HomebrewApp *) realloc(*items, sizeof(HomebrewApp) * (*cap));
                if (!newItems) {
                    free(full);
                    break;
                }
                *items = newItems;
            }
            (*items)[*count].path    = full;
            (*items)[*count].is_wuhb = ends_with(ent->d_name, ".wuhb");
            char *meta_name          = NULL;
            if (read_meta_name(dir_path, &meta_name)) {
                (*items)[*count].name = meta_name;
            } else {
                (*items)[*count].name = dupstr(ent->d_name);
            }
            (*count)++;
        } else if (S_ISDIR(st.st_mode)) {
            scan_dir(full, items, count, cap);
            free(full);
        } else {
            free(full);
            continue;
        }
    }
    closedir(dir);
}
 
 bool hb_scan_apps(const char *root_path, HomebrewAppList *out_list) {
     if (!out_list) return false;
     out_list->items = NULL;
     out_list->count = 0;
 
    DIR *root = opendir(root_path);
    if (!root) {
         DEBUG_FUNCTION_LINE_ERR("Failed to open apps root: %s", root_path);
         return false;
     }
    closedir(root);
 
     size_t cap = 16;
     HomebrewApp *items = (HomebrewApp *) malloc(sizeof(HomebrewApp) * cap);
     if (!items) {
         return false;
     }
 
    scan_dir(root_path, &items, &out_list->count, &cap);
 
     if (out_list->count == 0) {
         free(items);
         return false;
     }
     out_list->items = items;
     DEBUG_FUNCTION_LINE_INFO("Found %d homebrew apps", (int) out_list->count);
     return true;
 }
 
 void hb_free_app_list(HomebrewAppList *list) {
     if (!list || !list->items) return;
     for (size_t i = 0; i < list->count; i++) {
         free(list->items[i].name);
         free(list->items[i].path);
     }
     free(list->items);
     list->items = NULL;
     list->count = 0;
 }
