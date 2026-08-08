#include "cache.h"
#include "../utils/logger.h"
#include <string.h>
#include <stdlib.h>

extern bool IsPathFavorited(const char *path);

static HomebrewAppList s_cachedList = {NULL, 0};
static bool s_cacheValid = false;
static bool s_wuhbOnly = false;

bool Cache_Init(void) {
    s_cachedList.items = NULL;
    s_cachedList.count = 0;
    s_cacheValid = false;
    return true;
}

static void cache_filter(void) {
    if (!s_wuhbOnly) return;
    if (!s_cachedList.items || s_cachedList.count == 0) return;

    size_t out = 0;
    for (size_t i = 0; i < s_cachedList.count; i++) {
        if (!s_cachedList.items[i].is_wuhb) {
            free(s_cachedList.items[i].name);
            free(s_cachedList.items[i].path);
            continue;
        }
        if (out != i) {
            s_cachedList.items[out] = s_cachedList.items[i];
        }
        out++;
    }
    s_cachedList.count = out;
    if (out == 0) {
        free(s_cachedList.items);
        s_cachedList.items = NULL;
    }
}

bool Cache_Refresh(void) {
    hb_free_app_list(&s_cachedList);
    s_cachedList.items = NULL;
    s_cachedList.count = 0;

    if (!hb_scan_apps("sd:/wiiu/apps", &s_cachedList)) {
        if (!hb_scan_apps("fs:/vol/external01/wiiu/apps", &s_cachedList)) {
            DEBUG_FUNCTION_LINE_WARN("No homebrew apps found");
            s_cacheValid = true;
            return false;
        }
    }

    cache_filter();
    s_cacheValid = true;
    DEBUG_FUNCTION_LINE_INFO("Cached %d homebrew apps", (int)s_cachedList.count);
    return true;
}

void Cache_SetWuhbOnly(bool only) {
    if (s_wuhbOnly == only) return;
    s_wuhbOnly = only;
    Cache_Refresh();
}

const HomebrewAppList *Cache_GetAppList(void) {
    return &s_cachedList;
}

void Cache_SortByFavorites(void) {
    if (!s_cachedList.items || s_cachedList.count < 2) return;

    HomebrewApp *sorted = (HomebrewApp *)malloc(s_cachedList.count * sizeof(HomebrewApp));
    if (!sorted) return;

    size_t out = 0;
    for (size_t i = 0; i < s_cachedList.count; i++) {
        if (IsPathFavorited(s_cachedList.items[i].path)) {
            sorted[out++] = s_cachedList.items[i];
            s_cachedList.items[i].name = NULL;
            s_cachedList.items[i].path = NULL;
        }
    }
    for (size_t i = 0; i < s_cachedList.count; i++) {
        if (s_cachedList.items[i].name) {
            sorted[out++] = s_cachedList.items[i];
        }
    }

    free(s_cachedList.items);
    s_cachedList.items = sorted;
}

void Cache_Free(void) {
    hb_free_app_list(&s_cachedList);
    s_cacheValid = false;
}
