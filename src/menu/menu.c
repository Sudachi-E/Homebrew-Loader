#include "menu.h"
#include "cache.h"
#include "../homebrew/scanner.h"
#include "../utils/schrift.h"
#include <coreinit/screen.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/memory.h>
#include <coreinit/memdefaultheap.h>
#include <memory/mappedmemory.h>
#include <gx2/surface.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <vpad/input.h>
#include <rpxloader/rpxloader.h>
#include <notifications/notifications.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern void RegisterQuickFavorite(const char *identifier, const char *displayName, const char *path);
extern void RemoveQuickFavoriteByPath(const char *path);
extern int s_quickFavCount;
extern bool IsPathFavorited(const char *path);

extern uint32_t __OSPhysicalToEffectiveUncached(uint32_t physicalAddress);

typedef struct {
    void *buffer;
    uint32_t buffer_size;
    int32_t mode;
    GX2SurfaceFormat surface_format;
    GX2BufferingMode buffering_mode;
} StoredBuffer;

extern StoredBuffer *Menu_GetStoredTVBuffer(void);
extern StoredBuffer *Menu_GetStoredDRCBuffer(void);

#define DC_REGISTER_BASE    0x0C200000
#define DC_SCREEN_OFFSET    0x200
#define D1GRPH_ENABLE_REG   0x1840
#define D1GRPH_CONTROL_REG  0x1841
#define D1GRPH_PITCH_REG    0x1848
#define D1OVL_PITCH_REG     0x1866
#define D1GRPH_X_END_REG    0x184d

typedef struct {
    uint32_t tvEnable, tvControl, tvPitch, tvOverlayPitch;
    uint32_t drcEnable, drcControl, drcPitch, drcOverlayPitch;
} DCRegisters;

static uint32_t dc_read32(OSScreenID screen, uint32_t idx) {
    if (OSIsECOMode()) return 0;
    volatile uint32_t *regs = (uint32_t *)__OSPhysicalToEffectiveUncached(DC_REGISTER_BASE);
    return regs[idx + (screen * DC_SCREEN_OFFSET)];
}

static void dc_write32(OSScreenID screen, uint32_t idx, uint32_t val) {
    if (OSIsECOMode()) return;
    volatile uint32_t *regs = (uint32_t *)__OSPhysicalToEffectiveUncached(DC_REGISTER_BASE);
    regs[idx + (screen * DC_SCREEN_OFFSET)] = val;
}

static void dc_save(DCRegisters *r) {
    r->tvEnable       = dc_read32(SCREEN_TV, D1GRPH_ENABLE_REG);
    r->tvControl      = dc_read32(SCREEN_TV, D1GRPH_CONTROL_REG);
    r->tvPitch        = dc_read32(SCREEN_TV, D1GRPH_PITCH_REG);
    r->tvOverlayPitch = dc_read32(SCREEN_TV, D1OVL_PITCH_REG);
    r->drcEnable       = dc_read32(SCREEN_DRC, D1GRPH_ENABLE_REG);
    r->drcControl      = dc_read32(SCREEN_DRC, D1GRPH_CONTROL_REG);
    r->drcPitch        = dc_read32(SCREEN_DRC, D1GRPH_PITCH_REG);
    r->drcOverlayPitch = dc_read32(SCREEN_DRC, D1OVL_PITCH_REG);
}

static void dc_restore(const DCRegisters *r) {
    dc_write32(SCREEN_TV, D1GRPH_ENABLE_REG, r->tvEnable);
    dc_write32(SCREEN_TV, D1GRPH_CONTROL_REG, r->tvControl);
    dc_write32(SCREEN_TV, D1GRPH_PITCH_REG, r->tvPitch);
    dc_write32(SCREEN_TV, D1OVL_PITCH_REG, r->tvOverlayPitch);
    dc_write32(SCREEN_DRC, D1GRPH_ENABLE_REG, r->drcEnable);
    dc_write32(SCREEN_DRC, D1GRPH_CONTROL_REG, r->drcControl);
    dc_write32(SCREEN_DRC, D1GRPH_PITCH_REG, r->drcPitch);
    dc_write32(SCREEN_DRC, D1OVL_PITCH_REG, r->drcOverlayPitch);
}

static void *tvFramebuffer = NULL;
static void *drcFramebuffer = NULL;
static uint32_t tvFramebufferSize = 0;
static uint32_t drcFramebufferSize = 0;
static DCRegisters savedDC;
static int homeWasEnabled = 0;
static bool tvFromMapped = false;
static bool drcFromMapped = false;
static bool tvFromDefaultHeap = false;
static bool drcFromDefaultHeap = false;
static uint32_t s_tvWidth = 1280;
static uint32_t s_tvHeight = 720;
static float s_tvScale = 1.5f;
static bool s_isBackBuffer = false;

static int s_rowHeight = 24;
#define DRC_VISIBLE_W 854
#define DRC_VISIBLE_H 480
#define DRC_STRIDE 896
#define TV_WIDTH_DRC_REF 854

static void detect_tv_scale(void) {
    if (s_tvWidth >= 1920) {
        s_tvHeight = 1080;
    } else {
        s_tvHeight = 720;
    }
    s_tvScale = (float)s_tvWidth / (float)TV_WIDTH_DRC_REF;
}

static void dc_set_pitch(OSScreenID screen, uint16_t pitch) {
    dc_write32(screen, D1GRPH_PITCH_REG, pitch);
    dc_write32(screen, D1OVL_PITCH_REG, pitch);
}

static void detect_tv_width(void) {
    uint32_t xEnd = dc_read32(SCREEN_TV, D1GRPH_X_END_REG);
    switch (xEnd) {
        case 640:  s_tvWidth = 640;  break;
        case 854:
        case 896:  s_tvWidth = 896;  break;
        case 1280: s_tvWidth = 1280; break;
        case 1920: s_tvWidth = 1920; break;
        default:   s_tvWidth = xEnd; break;
    }
    detect_tv_scale();
}

static void detect_backbuffer(void) {
    if (!tvFramebuffer || tvFramebufferSize == 0) return;
    uint32_t *buf = (uint32_t *)tvFramebuffer;
    uint32_t saved = buf[0];
    OSScreenPutPixelEx(SCREEN_TV, 0, 0, 0xABCDEF90);
    s_isBackBuffer = (buf[0] != 0xABCDEF90);
    buf[0] = saved;
}

static void fb_put_pixel(int x, int y, uint32_t color) {
    if (drcFramebuffer && drcFramebufferSize > 0) {
        if (x >= 0 && x < DRC_VISIBLE_W && y >= 0 && y < DRC_VISIBLE_H) {
            uint32_t half = drcFramebufferSize / 2;
            uint32_t *buf = (uint32_t *)((uint8_t *)drcFramebuffer + (s_isBackBuffer ? half : 0));
            buf[y * DRC_STRIDE + x] = color;
        }
    }
    if (tvFramebuffer && tvFramebufferSize > 0) {
        int startX = (int)(x * s_tvScale);
        int startY = (int)(y * s_tvScale);
        int endX = (int)((x + 1) * s_tvScale);
        int endY = (int)((y + 1) * s_tvScale);
        uint32_t half = tvFramebufferSize / 2;
        uint32_t *buf = (uint32_t *)((uint8_t *)tvFramebuffer + (s_isBackBuffer ? half : 0));
        for (int yy = startY; yy < endY; yy++) {
            if (yy < 0 || yy >= (int)s_tvHeight) continue;
            for (int xx = startX; xx < endX; xx++) {
                if (xx < 0 || xx >= (int)s_tvWidth) continue;
                buf[yy * s_tvWidth + xx] = color;
            }
        }
    }
}

static void fb_put_pixel_alpha(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
    if (alpha == 0) return;
    if (drcFramebuffer && drcFramebufferSize > 0) {
        if (x >= 0 && x < DRC_VISIBLE_W && y >= 0 && y < DRC_VISIBLE_H) {
            uint32_t half = drcFramebufferSize / 2;
            uint32_t *buf = (uint32_t *)((uint8_t *)drcFramebuffer + (s_isBackBuffer ? half : 0));
            uint32_t idx = (uint32_t)(y * DRC_STRIDE + x);
            if (alpha == 0xFF) {
                buf[idx] = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0x00;
            } else {
                uint32_t cur = buf[idx];
                uint32_t inv = 255 - alpha;
                uint32_t nr = ((uint32_t)r * alpha + ((cur >> 24) & 0xFF) * inv) / 255;
                uint32_t ng = ((uint32_t)g * alpha + ((cur >> 16) & 0xFF) * inv) / 255;
                uint32_t nb = ((uint32_t)b * alpha + ((cur >> 8) & 0xFF) * inv) / 255;
                buf[idx] = (nr << 24) | (ng << 16) | (nb << 8) | 0x00;
            }
        }
    }
    if (tvFramebuffer && tvFramebufferSize > 0) {
        int startX = (int)(x * s_tvScale);
        int startY = (int)(y * s_tvScale);
        int endX = (int)((x + 1) * s_tvScale);
        int endY = (int)((y + 1) * s_tvScale);
        uint32_t half = tvFramebufferSize / 2;
        uint32_t *buf = (uint32_t *)((uint8_t *)tvFramebuffer + (s_isBackBuffer ? half : 0));
        for (int yy = startY; yy < endY; yy++) {
            if (yy < 0 || yy >= (int)s_tvHeight) continue;
            for (int xx = startX; xx < endX; xx++) {
                if (xx < 0 || xx >= (int)s_tvWidth) continue;
                uint32_t idx = (uint32_t)(yy * s_tvWidth + xx);
                if (alpha == 0xFF) {
                    buf[idx] = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0x00;
                } else {
                    uint32_t cur = buf[idx];
                    uint32_t inv = 255 - alpha;
                    uint32_t nr = ((uint32_t)r * alpha + ((cur >> 24) & 0xFF) * inv) / 255;
                    uint32_t ng = ((uint32_t)g * alpha + ((cur >> 16) & 0xFF) * inv) / 255;
                    uint32_t nb = ((uint32_t)b * alpha + ((cur >> 8) & 0xFF) * inv) / 255;
                    buf[idx] = (nr << 24) | (ng << 16) | (nb << 8) | 0x00;
                }
            }
        }
    }
}

static void fb_clear(void) {
    if (drcFramebuffer && drcFramebufferSize > 0) {
        uint32_t half = drcFramebufferSize / 2;
        uint32_t *buf = (uint32_t *)((uint8_t *)drcFramebuffer + (s_isBackBuffer ? half : 0));
        uint32_t pixels = half / 4;
        for (uint32_t i = 0; i < pixels; i++) buf[i] = 0x1E1E2E00;
    }
    if (tvFramebuffer && tvFramebufferSize > 0) {
        uint32_t half = tvFramebufferSize / 2;
        uint32_t *buf = (uint32_t *)((uint8_t *)tvFramebuffer + (s_isBackBuffer ? half : 0));
        uint32_t pixels = half / 4;
        for (uint32_t i = 0; i < pixels; i++) buf[i] = 0x1E1E2E00;
    }
}

#define HEADER_ROW      0
#define LIST_START_ROW  2
#define FOOTER_ROW      19

#define COLOR_WHITE     0xFFFFFF00
#define COLOR_BLUE      0x89B4FA00
#define COLOR_YELLOW    0xF9E2AF00
#define COLOR_GREEN     0xA6E3A100
#define COLOR_GRAY      0x88888800
#define COLOR_BG        0x1E1E2E00
#define COLOR_HIGHLIGHT 0x45475A00

static SFT s_sft;
static bool s_fontOk = false;

#define GLYPH_CACHE_SIZE 128
typedef struct {
    uint32_t codepoint;
    bool valid;
    SFT_GMetrics metrics;
    uint8_t *pixels;
    uint16_t texWidth;
    uint16_t texHeight;
} GlyphEntry;
static GlyphEntry s_glyphCache[GLYPH_CACHE_SIZE];
static int s_glyphCount = 0;

static int s_charWidth = 11;
static int s_fontPixelHeight = 18;
static int s_ascenderPx = 14;

static uint32_t utf8_decode(const char **s) {
    unsigned char c = (unsigned char)**s;
    if (c < 0x80) {
        (*s)++;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        unsigned char c2 = (unsigned char)(*s)[1];
        *s += 2;
        if ((c2 & 0xC0) != 0x80) return 0xFFFD;
        return ((c & 0x1F) << 6) | (c2 & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {
        unsigned char c2 = (unsigned char)(*s)[1];
        unsigned char c3 = (unsigned char)(*s)[2];
        *s += 3;
        if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return 0xFFFD;
        return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
    }
    if ((c & 0xF8) == 0xF0) {
        unsigned char c2 = (unsigned char)(*s)[1];
        unsigned char c3 = (unsigned char)(*s)[2];
        unsigned char c4 = (unsigned char)(*s)[3];
        *s += 4;
        if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80) return 0xFFFD;
        return ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
    }
    (*s)++;
    return 0xFFFD;
}

static void glyph_cache_clear(void) {
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        free(s_glyphCache[i].pixels);
    }
    memset(s_glyphCache, 0, sizeof(s_glyphCache));
    s_glyphCount = 0;
}

static void init_font(void) {
    void *fontData;
    uint32_t fontSize;
    OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &fontData, &fontSize);
    if (!fontData || fontSize == 0) return;

    SFT_Font *font = sft_loadmem(fontData, fontSize);
    if (!font) return;

    memset(&s_sft, 0, sizeof(s_sft));
    s_sft.font = font;
    s_sft.xScale = 18;
    s_sft.yScale = 18;
    s_sft.flags = SFT_DOWNWARD_Y;

    SFT_Glyph gid;
    SFT_GMetrics m;
    if (sft_lookup(&s_sft, (SFT_UChar)'n', &gid) >= 0 && sft_gmetrics(&s_sft, gid, &m) >= 0) {
        s_charWidth = (int)(m.advanceWidth + 0.5);
    }
    if (s_charWidth < 8) s_charWidth = 8;
    if (s_charWidth > 16) s_charWidth = 16;

    SFT_LMetrics lm;
    if (sft_lmetrics(&s_sft, &lm) >= 0) {
        s_fontPixelHeight = (int)(lm.ascender - lm.descender + 0.5);
        s_ascenderPx = (int)(lm.ascender + 0.5);
    }
    if (s_fontPixelHeight < 12) s_fontPixelHeight = 12;
    if (s_fontPixelHeight > 30) s_fontPixelHeight = 30;
    if (s_ascenderPx < 8) s_ascenderPx = 8;
    if (s_ascenderPx > 24) s_ascenderPx = 24;

    glyph_cache_clear();
    s_fontOk = true;
}

static void deinit_font(void) {
    glyph_cache_clear();
    if (s_sft.font) {
        sft_freefont(s_sft.font);
        s_sft.font = NULL;
    }
    s_fontOk = false;
}

static GlyphEntry *get_glyph(uint32_t codepoint) {
    if (!s_fontOk) return NULL;
    for (int i = 0; i < s_glyphCount; i++) {
        if (s_glyphCache[i].valid && s_glyphCache[i].codepoint == codepoint)
            return &s_glyphCache[i];
    }
    if (s_glyphCount >= GLYPH_CACHE_SIZE) {
        s_glyphCount--;
        free(s_glyphCache[s_glyphCount].pixels);
        s_glyphCache[s_glyphCount].valid = false;
    }
    GlyphEntry *entry = &s_glyphCache[s_glyphCount++];
    entry->codepoint = codepoint;
    entry->valid = false;
    entry->pixels = NULL;
    SFT_Glyph gid;
    if (sft_lookup(&s_sft, (SFT_UChar)codepoint, &gid) < 0) return entry;
    if (sft_gmetrics(&s_sft, gid, &entry->metrics) < 0) return entry;
    entry->texWidth = (entry->metrics.minWidth + 3) & ~3;
    entry->texHeight = entry->metrics.minHeight;
    if (entry->texWidth < 4) entry->texWidth = 4;
    if (entry->texHeight < 4) entry->texHeight = 4;
    entry->pixels = (uint8_t *)malloc(entry->texWidth * entry->texHeight);
    if (!entry->pixels) return entry;
    memset(entry->pixels, 0, entry->texWidth * entry->texHeight);
    SFT_Image img = { .pixels = entry->pixels, .width = entry->texWidth, .height = entry->texHeight };
    if (sft_render(&s_sft, gid, img) < 0) {
        free(entry->pixels);
        entry->pixels = NULL;
        return entry;
    }
    entry->valid = true;
    return entry;
}

static int render_glyph(int x, int y, uint32_t codepoint, uint32_t color) {
    GlyphEntry *g = get_glyph(codepoint);
    if (!g || !g->valid || !g->pixels) {
        if (!s_fontOk) return 8;
        return (int)(g ? g->metrics.advanceWidth + 0.5 : 8);
    }
    uint8_t fgR = (color >> 24) & 0xFF;
    uint8_t fgG = (color >> 16) & 0xFF;
    uint8_t fgB = (color >> 8) & 0xFF;
    int bx = x + (int)(g->metrics.leftSideBearing + 0.5);
    int by = y + g->metrics.yOffset;
    uint8_t *src = g->pixels;
    for (int j = 0; j < g->texHeight; j++) {
        for (int i = 0; i < g->texWidth; i++) {
            uint8_t alpha = src[j * g->texWidth + i];
            if (alpha == 0) continue;
            fb_put_pixel_alpha(bx + i, by + j, fgR, fgG, fgB, alpha);
        }
    }
    return (int)(g->metrics.advanceWidth + 0.5);
}

static int text_width(const char *text) {
    if (!text) return 0;
    int w = 0;
    while (*text) {
        uint32_t cp = utf8_decode(&text);
        GlyphEntry *g = get_glyph(cp);
        if (g && g->valid)
            w += (int)(g->metrics.advanceWidth + 0.5);
        else
            w += 8;
    }
    return w;
}

static void draw_text(int x, int y, const char *text, uint32_t color) {
    if (!text) return;
    int penX = x;
    while (*text) {
        uint32_t cp = utf8_decode(&text);
        penX += render_glyph(penX, y, cp, color);
    }
}

static void draw_text_colored_part(int x, int y, const char *text, int numColored, uint32_t color) {
    if (!text) return;
    int penX = x;
    int count = 0;
    while (*text) {
        uint32_t cp = utf8_decode(&text);
        uint32_t c = (count < numColored) ? color : COLOR_WHITE;
        penX += render_glyph(penX, y, cp, c);
        count++;
    }
}

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            fb_put_pixel(xx, yy, color);
        }
    }
}

static int row_y(int row) {
    return row * s_rowHeight + ((s_rowHeight - s_fontPixelHeight) / 2) + s_ascenderPx;
}

static void normalize_app_name(char *buf, size_t bufSize, const char *name) {
    if (!name || !buf || bufSize == 0) return;
    size_t len = strlen(name);
    size_t maxLen = bufSize - 1;
    size_t copyLen = (len < maxLen) ? len : maxLen;
    memcpy(buf, name, copyLen);
    buf[copyLen] = 0;
    while (copyLen > 0 && (buf[copyLen - 1] == '\n' || buf[copyLen - 1] == '\r')) {
        buf[--copyLen] = 0;
    }
}

bool Menu_Init(void) {
    Cache_Init();
    Cache_Refresh();
    return true;
}

static bool Menu_InitRenderer(void) {
    homeWasEnabled = OSIsHomeButtonMenuEnabled();
    dc_save(&savedDC);

    OSScreenInit();
    tvFramebufferSize = OSScreenGetBufferSizeEx(SCREEN_TV);
    drcFramebufferSize = OSScreenGetBufferSizeEx(SCREEN_DRC);

    tvFramebuffer = NULL;
    drcFramebuffer = NULL;
    tvFromMapped = false;
    drcFromMapped = false;
    tvFromDefaultHeap = false;
    drcFromDefaultHeap = false;

    StoredBuffer *storedTV = Menu_GetStoredTVBuffer();
    StoredBuffer *storedDRC = Menu_GetStoredDRCBuffer();

    if (MEMAllocFromMappedMemoryForGX2Ex) {
        if (tvFramebufferSize > 0) {
            tvFramebuffer = MEMAllocFromMappedMemoryForGX2Ex(tvFramebufferSize, 0x100);
            if (tvFramebuffer) tvFromMapped = true;
        }
        if (drcFramebufferSize > 0) {
            drcFramebuffer = MEMAllocFromMappedMemoryForGX2Ex(drcFramebufferSize, 0x100);
            if (drcFramebuffer) drcFromMapped = true;
        }
    }

    if (!tvFramebuffer || !drcFramebuffer) {
        if (!tvFramebuffer && storedTV && storedTV->buffer && storedTV->buffer_size >= tvFramebufferSize) {
            tvFramebuffer = storedTV->buffer;
            tvFromDefaultHeap = false;
        }
        if (!drcFramebuffer && storedDRC && storedDRC->buffer && storedDRC->buffer_size >= drcFramebufferSize) {
            drcFramebuffer = storedDRC->buffer;
            drcFromDefaultHeap = false;
        }
    }

    if (!tvFramebuffer || !drcFramebuffer) {
        if (tvFramebuffer) {
            if (tvFromMapped && MEMFreeToMappedMemory) MEMFreeToMappedMemory(tvFramebuffer);
            else if (tvFromDefaultHeap && MEMFreeToDefaultHeap) MEMFreeToDefaultHeap(tvFramebuffer);
        }
        if (drcFramebuffer) {
            if (drcFromMapped && MEMFreeToMappedMemory) MEMFreeToMappedMemory(drcFramebuffer);
            else if (drcFromDefaultHeap && MEMFreeToDefaultHeap) MEMFreeToDefaultHeap(drcFramebuffer);
        }
        tvFramebuffer = NULL;
        drcFramebuffer = NULL;
        tvFramebufferSize = 0;
        drcFramebufferSize = 0;
        tvFromMapped = false;
        drcFromMapped = false;
        tvFromDefaultHeap = false;
        drcFromDefaultHeap = false;
        if (Menu_NotificationModuleLoaded()) {
            NotificationModule_SetDefaultValue(NOTIFICATION_MODULE_NOTIFICATION_TYPE_ERROR, NOTIFICATION_MODULE_DEFAULT_OPTION_KEEP_UNTIL_SHOWN, true);
            NotificationModule_SetDefaultValue(NOTIFICATION_MODULE_NOTIFICATION_TYPE_ERROR, NOTIFICATION_MODULE_DEFAULT_OPTION_DURATION_BEFORE_FADE_OUT, 5.0);
            NotificationModule_AddInfoNotification("Cannot open menu - not enough memory");
        }
        OSEnableHomeButtonMenu(homeWasEnabled);
        dc_restore(&savedDC);
        return false;
    }

    if (tvFramebuffer) OSScreenSetBufferEx(SCREEN_TV, tvFramebuffer);
    if (drcFramebuffer) OSScreenSetBufferEx(SCREEN_DRC, drcFramebuffer);

    for (int i = 0; i < 2; i++) {
        if (tvFramebuffer) OSScreenClearBufferEx(SCREEN_TV, 0);
        if (drcFramebuffer) OSScreenClearBufferEx(SCREEN_DRC, 0);
        if (tvFramebuffer) {
            DCFlushRange(tvFramebuffer, tvFramebufferSize);
            OSScreenFlipBuffersEx(SCREEN_TV);
        }
        if (drcFramebuffer) {
            DCFlushRange(drcFramebuffer, drcFramebufferSize);
            OSScreenFlipBuffersEx(SCREEN_DRC);
        }
    }

    if (tvFramebuffer) OSScreenEnableEx(SCREEN_TV, TRUE);
    if (drcFramebuffer) OSScreenEnableEx(SCREEN_DRC, TRUE);

    detect_tv_width();
    if (tvFramebuffer) dc_set_pitch(SCREEN_TV, s_tvWidth);

    OSEnableHomeButtonMenu(FALSE);
    return true;
}

static void Menu_DeinitRenderer(void) {
    OSEnableHomeButtonMenu(homeWasEnabled);
    dc_restore(&savedDC);

    if (tvFramebuffer && tvFromMapped && MEMFreeToMappedMemory) {
        MEMFreeToMappedMemory(tvFramebuffer);
    } else if (tvFramebuffer && tvFromDefaultHeap && MEMFreeToDefaultHeap) {
        MEMFreeToDefaultHeap(tvFramebuffer);
    }
    if (drcFramebuffer && drcFromMapped && MEMFreeToMappedMemory) {
        MEMFreeToMappedMemory(drcFramebuffer);
    } else if (drcFramebuffer && drcFromDefaultHeap && MEMFreeToDefaultHeap) {
        MEMFreeToDefaultHeap(drcFramebuffer);
    }
    tvFramebuffer = NULL;
    drcFramebuffer = NULL;
    tvFramebufferSize = 0;
    drcFramebufferSize = 0;
    tvFromMapped = false;
    drcFromMapped = false;
    tvFromDefaultHeap = false;
    drcFromDefaultHeap = false;
}

void Menu_Open(void) {
    if (!Menu_InitRenderer()) return;

    const HomebrewAppList *list = Cache_GetAppList();
    if (!list || list->count == 0) {
        Cache_Refresh();
        list = Cache_GetAppList();
        if (!list || list->count == 0) {
            Menu_DeinitRenderer();
            return;
        }
    }
    Cache_SortByFavorites();
    list = Cache_GetAppList();

    int maxVisible = FOOTER_ROW - LIST_START_ROW;

    init_font();

    int selectedIndex = 0;
    int scrollOffset = 0;
    int menuOpen = 1;

    while (menuOpen) {
        GX2WaitForVsync();
        detect_backbuffer();

        fb_clear();

        draw_text(8, row_y(HEADER_ROW), "Homebrew Menu", COLOR_GREEN);
        {
            char countStr[32];
            snprintf(countStr, sizeof(countStr), "%d apps", (int)list->count);
            int cw = text_width(countStr);
            draw_text(DRC_VISIBLE_W - 8 - cw, row_y(HEADER_ROW), countStr, COLOR_BLUE);
        }

        for (int i = 0; i < maxVisible; i++) {
            int idx = scrollOffset + i;
            if (idx >= (int)list->count) break;

            const HomebrewApp *app = &list->items[idx];
            char name[128];
            normalize_app_name(name, sizeof(name), app->name ? app->name : "Unknown");

            int itemY = (LIST_START_ROW + i) * s_rowHeight;
            if (idx == selectedIndex) {
                draw_rect(0, itemY, DRC_VISIBLE_W - 16, s_rowHeight, COLOR_HIGHLIGHT);
            }

            char line[160];
            int lineY = row_y(LIST_START_ROW + i);
            if (IsPathFavorited(app->path)) {
                snprintf(line, sizeof(line), " * %s", name);
                draw_text_colored_part(8, lineY, line, 3, COLOR_YELLOW);
            } else {
                snprintf(line, sizeof(line), "   %s", name);
                draw_text(8, lineY, line, COLOR_WHITE);
            }
        }

        draw_text(8, row_y(FOOTER_ROW), "\xEE\x80\x80:Launch  \xEE\x80\x81:Close  \xEE\x80\x83:Fav  \xEE\x81\x86:Refresh  \xEE\x80\x84:Top  \xEE\x80\x85:Bottom  \xEE\x81\xBD:Nav", COLOR_WHITE);

        if ((int)list->count > maxVisible) {
            int scrollMax = (int)list->count - maxVisible;
            float pct = (scrollMax > 0) ? (float)scrollOffset / scrollMax : 0;
            int barRow = LIST_START_ROW + (int)(pct * (maxVisible - 2));
            draw_rect(DRC_VISIBLE_W - 20, barRow * s_rowHeight, 2, 2, COLOR_GRAY);
        }

        if (tvFramebuffer) {
            DCFlushRange(tvFramebuffer, tvFramebufferSize);
            OSScreenFlipBuffersEx(SCREEN_TV);
        }
        if (drcFramebuffer) {
            DCFlushRange(drcFramebuffer, drcFramebufferSize);
            OSScreenFlipBuffersEx(SCREEN_DRC);
        }

    VPADStatus vpad;
    VPADReadError err;
    int32_t read = VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
    if (read <= 0 || err != VPAD_READ_SUCCESS) {
        for (int retry = 0; retry < 5; retry++) {
            OSYieldThread();
            read = VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
            if (read > 0 && err == VPAD_READ_SUCCESS) break;
        }
        if (read <= 0 || err != VPAD_READ_SUCCESS) continue;
    }

    uint32_t pressed = vpad.trigger;
    uint32_t held = vpad.hold;

    static int s_repeatTimer = 0;
    int repeatMove = 0;

    if (pressed & VPAD_BUTTON_DOWN) {
        repeatMove = 1;
        s_repeatTimer = 8;
    } else if (pressed & VPAD_BUTTON_UP) {
        repeatMove = -1;
        s_repeatTimer = 8;
    } else if (s_repeatTimer > 0 && held & (VPAD_BUTTON_DOWN | VPAD_BUTTON_UP)) {
        s_repeatTimer--;
        if (s_repeatTimer == 0) {
            repeatMove = (held & VPAD_BUTTON_DOWN) ? 1 : -1;
            s_repeatTimer = 1;
        }
    } else {
        s_repeatTimer = 0;
    }

    if (repeatMove == 1) {
        if (selectedIndex < (int)list->count - 1) {
            selectedIndex++;
            if (selectedIndex - scrollOffset >= maxVisible) {
                scrollOffset++;
            }
        } else {
            selectedIndex = 0;
            scrollOffset = 0;
        }
    }

    if (repeatMove == -1) {
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset--;
            }
        } else {
            selectedIndex = (int)list->count - 1;
            if (selectedIndex > maxVisible) {
                scrollOffset = selectedIndex - maxVisible + 1;
            } else {
                scrollOffset = 0;
            }
        }
    }

        if (pressed & VPAD_BUTTON_B) {
            menuOpen = 0;
        }

        if (pressed & VPAD_BUTTON_A) {
            const HomebrewApp *app = &list->items[selectedIndex];

            RPXLoaderStatus initRes = RPXLoader_InitLibrary();
            if (initRes != RPX_LOADER_RESULT_SUCCESS) {
                continue;
            }

            const char *rp = app->path;
            if (strncmp(rp, "sd:/", 4) == 0) {
                rp += 4;
            } else if (strncmp(rp, "fs:/", 4) == 0) {
                const char *vol = strstr(rp, "/vol/external01/");
                if (vol) {
                    rp = vol + 16;
                }
            }
            const char *launchPath = rp;

            RPXLoader_LaunchHomebrew(launchPath);
            RPXLoader_DeInitLibrary();
            menuOpen = 0;
        }

        if (pressed & VPAD_BUTTON_Y) {
            const HomebrewApp *app = &list->items[selectedIndex];
            if (IsPathFavorited(app->path)) {
                RemoveQuickFavoriteByPath(app->path);
            } else {
                RegisterQuickFavorite(app->path, app->name ? app->name : "App", app->path);
            }
            Cache_SortByFavorites();
            list = Cache_GetAppList();
            selectedIndex = 0;
            scrollOffset = 0;
        }

        if (pressed & VPAD_BUTTON_MINUS) {
            Cache_Refresh();
            Cache_SortByFavorites();
            list = Cache_GetAppList();
            if (!list || list->count == 0) break;
            selectedIndex = 0;
            scrollOffset = 0;
        }

        if (pressed & VPAD_BUTTON_R) {
            selectedIndex = (int)list->count - 1;
            if (selectedIndex > maxVisible) {
                scrollOffset = selectedIndex - maxVisible + 1;
            } else {
                scrollOffset = 0;
            }
        }

        if (pressed & VPAD_BUTTON_L) {
            selectedIndex = 0;
            scrollOffset = 0;
        }
    }

    OSSleepTicks(OSMillisecondsToTicks(300));

    deinit_font();
    Menu_DeinitRenderer();
}
