#include "menu.h"
#include "cache.h"
#include "../homebrew/scanner.h"
#include "../utils/schrift.h"
#include <coreinit/screen.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/memory.h>
#include <coreinit/energysaver.h>
#include <coreinit/memdefaultheap.h>
#include <memory/mappedmemory.h>
#include <gx2/surface.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <vpad/input.h>
#include <padscore/kpad.h>
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
static uint32_t s_savedDrcDim = 0;
static bool tvFromMapped = false;
static bool drcFromMapped = false;
static bool tvFromDefaultHeap = false;
static bool drcFromDefaultHeap = false;
static uint32_t s_tvWidth = 1280;
static uint32_t s_tvHeight = 720;
static float s_tvScale = 1.5f;
static bool s_isBackBuffer = false;

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

#define COLOR_BG        0x1E1E2E00
#define COLOR_TEXT      0xFFFFFF00
#define COLOR_TEXT2     0x88888800
#define COLOR_BORDER    0x45475A00
#define COLOR_HIGHLIGHT 0x89B4FA00
#define COLOR_BAR       0x585B7000
#define COLOR_YELLOW    0xF9E2AF00

#define ITEM_BOX_X      16
#define ITEM_BOX_W      (DRC_VISIBLE_W - 16 * 2)
#define ITEM_BOX_H      44
#define ITEM_PITCH      50
#define ITEM_TEXT_BASELINE (8 + 24)
#define MAX_ITEMS_ON_SCREEN 8

#define TOP_BAR_LINE_Y  36
#define BOTTOM_BAR_LINE_Y (DRC_VISIBLE_H - 24 - 8 - 4)
#define FOOTER_BASELINE_Y (DRC_VISIBLE_H - 10)

static void fb_clear(void) {
    if (drcFramebuffer && drcFramebufferSize > 0) {
        uint32_t half = drcFramebufferSize / 2;
        uint32_t *buf = (uint32_t *)((uint8_t *)drcFramebuffer + (s_isBackBuffer ? half : 0));
        uint32_t pixels = half / 4;
        for (uint32_t i = 0; i < pixels; i++) buf[i] = COLOR_BG;
    }
    if (tvFramebuffer && tvFramebufferSize > 0) {
        uint32_t half = tvFramebufferSize / 2;
        uint32_t *buf = (uint32_t *)((uint8_t *)tvFramebuffer + (s_isBackBuffer ? half : 0));
        uint32_t pixels = half / 4;
        for (uint32_t i = 0; i < pixels; i++) buf[i] = COLOR_BG;
    }
}

static SFT s_sft;
static bool s_fontOk = false;

#define GLYPH_CACHE_SIZE 192
#define GLYPH_FALLBACK_ADVANCE 8
typedef struct {
    uint32_t codepoint;
    int size;
    bool valid;
    SFT_GMetrics metrics;
    uint8_t *pixels;
    uint16_t texWidth;
    uint16_t texHeight;
} GlyphEntry;
static GlyphEntry s_glyphCache[GLYPH_CACHE_SIZE];
static int s_glyphCount = 0;
static int s_fontSize = 18;

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
    s_fontSize = 18;
    s_sft.flags = SFT_DOWNWARD_Y;

    glyph_cache_clear();
    s_fontOk = true;
}

static void deinit_font(void) {
    glyph_cache_clear();
    if (s_sft.font) {
        sft_freefont(s_sft.font);
        s_sft.font = NULL;
    }
    s_fontSize = 18;
    s_fontOk = false;
}

static void set_font_size(uint32_t size) {
    if (!s_fontOk || size == (uint32_t)s_fontSize) return;
    s_fontSize = (int)size;
    s_sft.xScale = size;
    s_sft.yScale = size;
}

static GlyphEntry *get_glyph(uint32_t codepoint) {
    if (!s_fontOk) return NULL;
    for (int i = 0; i < s_glyphCount; i++) {
        if (s_glyphCache[i].valid && s_glyphCache[i].codepoint == codepoint &&
            s_glyphCache[i].size == s_fontSize)
            return &s_glyphCache[i];
    }
    if (s_glyphCount >= GLYPH_CACHE_SIZE) {
        s_glyphCount--;
        free(s_glyphCache[s_glyphCount].pixels);
        s_glyphCache[s_glyphCount].valid = false;
    }
    GlyphEntry *entry = &s_glyphCache[s_glyphCount++];
    entry->codepoint = codepoint;
    entry->size = s_fontSize;
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
        if (!s_fontOk) return GLYPH_FALLBACK_ADVANCE;
        return (int)(g ? g->metrics.advanceWidth + 0.5 : GLYPH_FALLBACK_ADVANCE);
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
            w += GLYPH_FALLBACK_ADVANCE;
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

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;

    if (drcFramebuffer && drcFramebufferSize > 0) {
        int x0 = x < 0 ? 0 : x;
        int x1 = x + w; if (x1 > DRC_VISIBLE_W) x1 = DRC_VISIBLE_W;
        int y0 = y < 0 ? 0 : y;
        int y1 = y + h; if (y1 > DRC_VISIBLE_H) y1 = DRC_VISIBLE_H;
        if (x0 < x1 && y0 < y1) {
            uint32_t half = drcFramebufferSize / 2;
            uint32_t *buf = (uint32_t *)((uint8_t *)drcFramebuffer + (s_isBackBuffer ? half : 0));
            int rowlen = x1 - x0;
            for (int yy = y0; yy < y1; yy++) {
                uint32_t *row = &buf[yy * DRC_STRIDE + x0];
                for (int xx = 0; xx < rowlen; xx++) row[xx] = color;
            }
        }
    }

    if (tvFramebuffer && tvFramebufferSize > 0) {
        int startX = (int)(x * s_tvScale);
        int startY = (int)(y * s_tvScale);
        int endX   = (int)((x + w) * s_tvScale);
        int endY   = (int)((y + h) * s_tvScale);
        if (startX < 0) startX = 0;
        if (startY < 0) startY = 0;
        if (endX > (int)s_tvWidth)  endX = (int)s_tvWidth;
        if (endY > (int)s_tvHeight) endY = (int)s_tvHeight;
        if (startX < endX && startY < endY) {
            uint32_t half = tvFramebufferSize / 2;
            uint32_t *buf = (uint32_t *)((uint8_t *)tvFramebuffer + (s_isBackBuffer ? half : 0));
            int rowlen = endX - startX;
            for (int yy = startY; yy < endY; yy++) {
                uint32_t *row = &buf[yy * s_tvWidth + startX];
                for (int xx = 0; xx < rowlen; xx++) row[xx] = color;
            }
        }
    }
}

static void draw_rect_outline(int x, int y, int w, int h, int thickness, uint32_t color) {
    if (thickness <= 0) return;
    draw_rect(x, y, w, thickness, color);
    draw_rect(x, y + h - thickness, w, thickness, color);
    draw_rect(x, y + thickness, thickness, h - 2 * thickness, color);
    draw_rect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);
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
    IMGetDimEnableDRC(&s_savedDrcDim);
    IMSetDimEnableDRC(FALSE);

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
    IMSetDimEnableDRC(s_savedDrcDim);
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

    int maxVisible = MAX_ITEMS_ON_SCREEN;

    init_font();

    int selectedIndex = 0;
    int scrollOffset = 0;
    int menuOpen = 1;

    while (menuOpen) {
        GX2WaitForVsync();
        detect_backbuffer();

        fb_clear();

        set_font_size(24);
        draw_text(16, 6 + 24, "Homebrew Menu", COLOR_TEXT);
        {
            set_font_size(18);
            char countStr[64];
            snprintf(countStr, sizeof(countStr), "%d apps", (int)list->count);
            int cw = text_width(countStr);
            draw_text(DRC_VISIBLE_W - 16 - cw, 8 + 24, countStr, COLOR_TEXT);
        }
        draw_rect(8, TOP_BAR_LINE_Y, DRC_VISIBLE_W - 8 * 2, 3, COLOR_BAR);

        uint32_t yOffset = 8 + 24 + 8 + 4;
        for (int i = 0; i < maxVisible; i++) {
            int idx = scrollOffset + i;
            if (idx >= (int)list->count) break;

            const HomebrewApp *app = &list->items[idx];
            char name[128];
            normalize_app_name(name, sizeof(name), app->name ? app->name : "Unknown");

            bool isSelected = (idx == selectedIndex);
            bool isFav      = IsPathFavorited(app->path);

            if (isSelected) {
                draw_rect_outline(ITEM_BOX_X, yOffset, ITEM_BOX_W, ITEM_BOX_H, 4, COLOR_HIGHLIGHT);
            } else {
                draw_rect_outline(ITEM_BOX_X, yOffset, ITEM_BOX_W, ITEM_BOX_H, 2, COLOR_BORDER);
            }

            set_font_size(24);
            draw_text(ITEM_BOX_X * 2, yOffset + ITEM_TEXT_BASELINE, name, isFav ? COLOR_YELLOW : COLOR_TEXT);

            if (isFav) {
                set_font_size(18);
                int vw = text_width("Fav");
                draw_text(DRC_VISIBLE_W - ITEM_BOX_X * 2 - vw, yOffset + ITEM_TEXT_BASELINE, "Fav", COLOR_YELLOW);
                set_font_size(24);
            }

            yOffset += ITEM_PITCH;
        }

        draw_rect(8, BOTTOM_BAR_LINE_Y, DRC_VISIBLE_W - 8 * 2, 3, COLOR_BAR);
        set_font_size(18);

        draw_text(16, FOOTER_BASELINE_Y, "\xEE\x81\xBD Navigate", COLOR_TEXT);
        {
            const char *launchHint  = "\xEE\x80\x80 Launch  \xEE\x81\x85 Fav";
            const char *closeHint   = "\xEE\x80\x81 Close";
            const char *refreshHint = "\xEE\x81\x86 Refresh";
            const int rightW   = text_width(refreshHint);
            const int rightX   = DRC_VISIBLE_W - 16 - rightW;
            const int gap      = text_width("  ");
            const int closeW   = text_width(closeHint);
            const int launchW  = text_width(launchHint);
            draw_text(rightX - gap - closeW - gap - launchW, FOOTER_BASELINE_Y, launchHint, COLOR_TEXT);
            draw_text(rightX - gap - closeW, FOOTER_BASELINE_Y, closeHint, COLOR_TEXT);
            draw_text(rightX, FOOTER_BASELINE_Y, refreshHint, COLOR_TEXT);
        }

        set_font_size(24);
        if ((int)list->count > maxVisible && scrollOffset + maxVisible < (int)list->count) {
            draw_text(DRC_VISIBLE_W / 2 + 12, DRC_VISIBLE_H - 32, "\xEF\xB8\xBE", COLOR_TEXT);
        }
        if (scrollOffset > 0) {
            draw_text(DRC_VISIBLE_W / 2 + 12, 32 + 20, "\xEF\xB8\xBD", COLOR_TEXT);
        }

        if (tvFramebuffer) {
            DCFlushRange(tvFramebuffer, tvFramebufferSize);
            OSScreenFlipBuffersEx(SCREEN_TV);
        }
        if (drcFramebuffer) {
            DCFlushRange(drcFramebuffer, drcFramebufferSize);
            OSScreenFlipBuffersEx(SCREEN_DRC);
        }

    uint32_t pressed = 0;
    uint32_t held = 0;
    {
        VPADStatus vpad;
        VPADReadError err;
        int32_t read = VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
        if (read <= 0 || err != VPAD_READ_SUCCESS) {
            for (int retry = 0; retry < 5; retry++) {
                OSYieldThread();
                read = VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
                if (read > 0 && err == VPAD_READ_SUCCESS) break;
            }
        }
        if (read > 0 && err == VPAD_READ_SUCCESS) {
            pressed |= vpad.trigger;
            held    |= vpad.hold;
        }
    }

    for (KPADChan chan = WPAD_CHAN_0; chan <= WPAD_CHAN_3; chan++) {
        KPADStatus kpad;
        KPADError kerr;
        uint32_t read = KPADReadEx(chan, &kpad, 1, &kerr);
        if (read == 0 || kerr != KPAD_ERROR_OK) continue;

        uint32_t kp = 0, kh = 0;

        if (kpad.extensionType == WPAD_EXT_PRO_CONTROLLER) {
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_UP)    kp |= VPAD_BUTTON_UP;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_DOWN)  kp |= VPAD_BUTTON_DOWN;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_LEFT)  kp |= VPAD_BUTTON_LEFT;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_RIGHT) kp |= VPAD_BUTTON_RIGHT;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_A)     kp |= VPAD_BUTTON_A;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_B)     kp |= VPAD_BUTTON_B;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_PLUS)  kp |= VPAD_BUTTON_PLUS;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_MINUS) kp |= VPAD_BUTTON_MINUS;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_L)     kp |= VPAD_BUTTON_L;
            if (kpad.pro.trigger & WPAD_PRO_BUTTON_R)     kp |= VPAD_BUTTON_R;

            if (kpad.pro.hold & WPAD_PRO_BUTTON_UP)    kh |= VPAD_BUTTON_UP;
            if (kpad.pro.hold & WPAD_PRO_BUTTON_DOWN)  kh |= VPAD_BUTTON_DOWN;

            if (kpad.pro.leftStick.y >  0.5f) { kp |= VPAD_BUTTON_UP;   kh |= VPAD_BUTTON_UP;   }
            if (kpad.pro.leftStick.y < -0.5f) { kp |= VPAD_BUTTON_DOWN; kh |= VPAD_BUTTON_DOWN; }

        } else if (kpad.extensionType == WPAD_EXT_CORE ||
                   kpad.extensionType == WPAD_EXT_MPLUS) {
            if (kpad.trigger & WPAD_BUTTON_UP)    kp |= VPAD_BUTTON_UP;
            if (kpad.trigger & WPAD_BUTTON_DOWN)  kp |= VPAD_BUTTON_DOWN;
            if (kpad.trigger & WPAD_BUTTON_LEFT)  kp |= VPAD_BUTTON_LEFT;
            if (kpad.trigger & WPAD_BUTTON_RIGHT) kp |= VPAD_BUTTON_RIGHT;
            if (kpad.trigger & WPAD_BUTTON_A)     kp |= VPAD_BUTTON_A;
            if (kpad.trigger & WPAD_BUTTON_B)     kp |= VPAD_BUTTON_B;
            if (kpad.trigger & WPAD_BUTTON_PLUS)  kp |= VPAD_BUTTON_PLUS;
            if (kpad.trigger & WPAD_BUTTON_MINUS) kp |= VPAD_BUTTON_MINUS;
            if (kpad.trigger & WPAD_BUTTON_1)     kp |= VPAD_BUTTON_L;
            if (kpad.trigger & WPAD_BUTTON_2)     kp |= VPAD_BUTTON_R;

            if (kpad.hold & WPAD_BUTTON_UP)   kh |= VPAD_BUTTON_UP;
            if (kpad.hold & WPAD_BUTTON_DOWN) kh |= VPAD_BUTTON_DOWN;

        } else if (kpad.extensionType == WPAD_EXT_CLASSIC) {
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_UP)    kp |= VPAD_BUTTON_UP;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_DOWN)  kp |= VPAD_BUTTON_DOWN;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_LEFT)  kp |= VPAD_BUTTON_LEFT;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_RIGHT) kp |= VPAD_BUTTON_RIGHT;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_A)     kp |= VPAD_BUTTON_A;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_B)     kp |= VPAD_BUTTON_B;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_PLUS)  kp |= VPAD_BUTTON_PLUS;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_MINUS) kp |= VPAD_BUTTON_MINUS;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_L)     kp |= VPAD_BUTTON_L;
            if (kpad.classic.trigger & WPAD_CLASSIC_BUTTON_R)     kp |= VPAD_BUTTON_R;

            if (kpad.classic.hold & WPAD_CLASSIC_BUTTON_UP)    kh |= VPAD_BUTTON_UP;
            if (kpad.classic.hold & WPAD_CLASSIC_BUTTON_DOWN)  kh |= VPAD_BUTTON_DOWN;

            if (kpad.classic.leftStick.y >  0.5f) { kp |= VPAD_BUTTON_UP;   kh |= VPAD_BUTTON_UP;   }
            if (kpad.classic.leftStick.y < -0.5f) { kp |= VPAD_BUTTON_DOWN; kh |= VPAD_BUTTON_DOWN; }
        }

        pressed |= kp;
        held    |= kh;
    }

    if (pressed == 0 && held == 0) continue;

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

        if (pressed & VPAD_BUTTON_PLUS) {
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
