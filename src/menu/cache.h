#pragma once
#include "../homebrew/scanner.h"
#include <stdbool.h>

bool Cache_Init(void);
bool Cache_Refresh(void);
const HomebrewAppList *Cache_GetAppList(void);
void Cache_SortByFavorites(void);
void Cache_SetWuhbOnly(bool only);
void Cache_Free(void);
