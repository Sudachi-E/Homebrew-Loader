#include "menu.h"
#include "cache.h"
#include "../homebrew/scanner.h"
#include <coreinit/screen.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/memory.h>
#include <coreinit/memdefaultheap.h>
#include <memory/mappedmemory.h>
#include <gx2/surface.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <vpad/input.h>
#include <rpxloader/rpxloader.h>
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
static uint32_t s_drcWidth = 896;
static float s_tvScale = 1.5f;
static bool s_isBackBuffer = false;

static int s_fontScaleY = 2;
static int s_rowHeight = 24;
static int s_cols = 100;
static int s_rows = 20;

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

static void fb_put_pixel(int x, int y, uint32_t color) {
    if (drcFramebuffer && drcFramebufferSize > 0) {
        if (x >= 0 && x < 854 && y >= 0 && y < 480) {
            uint32_t half = drcFramebufferSize / 2;
            uint32_t *buf = (uint32_t *)((uint8_t *)drcFramebuffer + (s_isBackBuffer ? half : 0));
            buf[y * s_drcWidth + x] = color;
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

#define HEADER_ROW      0
#define LIST_START_ROW  2
#define FOOTER_ROW      (s_rows - 1)

#define COLOR_WHITE     0xFFFFFF00
#define COLOR_BLUE      0x89B4FA00
#define COLOR_YELLOW    0xF9E2AF00
#define COLOR_GREEN     0xA6E3A100
#define COLOR_GRAY      0x88888800
#define COLOR_BG        0x1E1E2E00
#define COLOR_HIGHLIGHT 0x45475A00

static const uint8_t FONT[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00},
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    {0x7C,0xCE,0xDE,0xF6,0xE6,0xC6,0x7C,0x00},
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00},
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00},
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x18,0x30},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    {0x3C,0x66,0xC0,0xCE,0x66,0x66,0x3A,0x00},
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00},
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00},
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x6C,0x38,0x00},
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00},
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00},
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x78},
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C},
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00},
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
    {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00},
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00},
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00},
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C},
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00},
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
};

static void draw_char(int sx, int sy, char c, uint32_t color) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;
    const uint8_t *glyph = FONT[idx];
    for (int gy = 0; gy < 8; gy++) {
        for (int gx = 0; gx < 8; gx++) {
            if (glyph[gy] & (0x80 >> gx)) {
                int px = sx + gx;
                int py = sy + gy * s_fontScaleY;
                fb_put_pixel(px, py, color);
                fb_put_pixel(px, py + 1, color);
            }
        }
    }
}

static size_t ascii_fold(char *out, size_t out_size, const char *in) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < out_size - 1; ) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            out[j++] = in[i++];
        } else if ((c & 0xE0) == 0xC0 && (in[i + 1] & 0xC0) == 0x80) {
            unsigned char c2 = (unsigned char)in[i + 1];
            if (c == 0xC3) {
                switch (c2) {
                    case 0x80 ... 0x86: out[j] = 'A'; break;
                    case 0x87:         out[j] = 'C'; break;
                    case 0x88 ... 0x8B: out[j] = 'E'; break;
                    case 0x8C ... 0x8F: out[j] = 'I'; break;
                    case 0x90:         out[j] = 'D'; break;
                    case 0x91:         out[j] = 'N'; break;
                    case 0x92 ... 0x96: case 0x98: out[j] = 'O'; break;
                    case 0x99 ... 0x9C: out[j] = 'U'; break;
                    case 0x9D:         out[j] = 'Y'; break;
                    case 0x9E:         out[j] = 'T'; break;
                    case 0x9F:         out[j] = 's'; break;
                    case 0xA0 ... 0xA6: out[j] = 'a'; break;
                    case 0xA7:         out[j] = 'c'; break;
                    case 0xA8 ... 0xAB: out[j] = 'e'; break;
                    case 0xAC ... 0xAF: out[j] = 'i'; break;
                    case 0xB0:         out[j] = 'd'; break;
                    case 0xB1:         out[j] = 'n'; break;
                    case 0xB2 ... 0xB6: case 0xB8: out[j] = 'o'; break;
                    case 0xB9 ... 0xBC: out[j] = 'u'; break;
                    case 0xBD:         out[j] = 'y'; break;
                    case 0xBE:         out[j] = 't'; break;
                    case 0xBF:         out[j] = 'y'; break;
                    default:           out[j] = '?'; break;
                }
            } else {
                out[j] = '?';
            }
            j++;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            out[j++] = '?';
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            out[j++] = '?';
            i += 4;
        } else {
            out[j++] = '?';
            i++;
        }
    }
    out[j] = '\0';
    return j;
}

static void draw_text(int col, int row, const char *text, uint32_t color) {
    if (!text) return;
    char folded[256];
    ascii_fold(folded, sizeof(folded), text);
    int sx = col * 8;
    int sy = row * s_rowHeight + ((s_rowHeight - 8 * s_fontScaleY) / 2);
    for (int i = 0; folded[i]; i++) {
        draw_char(sx + i * 8, sy, folded[i], color);
    }
}

static void draw_text_colored_part(int col, int row, const char *text, int numColored, uint32_t color) {
    if (!text) return;
    char folded[256];
    ascii_fold(folded, sizeof(folded), text);
    int sx = col * 8;
    int sy = row * s_rowHeight + ((s_rowHeight - 8 * s_fontScaleY) / 2);
    int i;
    for (i = 0; folded[i] && i < numColored; i++) {
        draw_char(sx + i * 8, sy, folded[i], color);
    }
    for (; folded[i]; i++) {
        draw_char(sx + i * 8, sy, folded[i], COLOR_WHITE);
    }
}

static void draw_rect(int col, int row, int width, int height, uint32_t color) {
    int sx = col * 8;
    int sy = row * s_rowHeight;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            fb_put_pixel(sx + x, sy + y, color);
        }
    }
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

void Menu_Open(void) {

    const HomebrewAppList *list = Cache_GetAppList();
    if (!list || list->count == 0) {
        Cache_Refresh();
        list = Cache_GetAppList();
        if (!list || list->count == 0) {
            return;
        }
    }
    Cache_SortByFavorites();
    list = Cache_GetAppList();

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

    if (!tvFramebuffer && MEMAllocFromDefaultHeapEx) {
        tvFramebuffer = MEMAllocFromDefaultHeapEx(tvFramebufferSize, 0x100);
        if (tvFramebuffer) tvFromDefaultHeap = true;
    }
    if (!drcFramebuffer && MEMAllocFromDefaultHeapEx) {
        drcFramebuffer = MEMAllocFromDefaultHeapEx(drcFramebufferSize, 0x100);
        if (drcFramebuffer) drcFromDefaultHeap = true;
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

    if (!tvFramebuffer && !drcFramebuffer) {
        OSEnableHomeButtonMenu(homeWasEnabled);
        dc_restore(&savedDC);
        return;
    }

    if (tvFramebuffer) OSScreenSetBufferEx(SCREEN_TV, tvFramebuffer);
    if (drcFramebuffer) OSScreenSetBufferEx(SCREEN_DRC, drcFramebuffer);

    for (int i = 0; i < 2; i++) {
        if (tvFramebuffer) {
            OSScreenClearBufferEx(SCREEN_TV, 0);
        }
        if (drcFramebuffer) {
            OSScreenClearBufferEx(SCREEN_DRC, 0);
        }
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
    if (tvFramebuffer) {
        dc_set_pitch(SCREEN_TV, s_tvWidth);
    }
    int maxVisible = s_rows - LIST_START_ROW - 1;
    OSEnableHomeButtonMenu(FALSE);

    int selectedIndex = 0;
    int scrollOffset = 0;
    int menuOpen = 1;

    while (menuOpen) {
        GX2WaitForVsync();
        detect_backbuffer();

        fb_clear();

        draw_text(1, HEADER_ROW, "Homebrew Menu", COLOR_GREEN);
        {
            char countStr[32];
            snprintf(countStr, sizeof(countStr), "%d apps", (int)list->count);
            draw_text(s_cols - (int)strlen(countStr) - 1, HEADER_ROW, countStr, COLOR_BLUE);
        }

        for (int i = 0; i < maxVisible; i++) {
            int idx = scrollOffset + i;
            if (idx >= (int)list->count) break;

            const HomebrewApp *app = &list->items[idx];
            char name[128];
            normalize_app_name(name, sizeof(name), app->name ? app->name : "Unknown");

            if (idx == selectedIndex) {
                draw_rect(0, LIST_START_ROW + i, s_cols * 8, s_rowHeight, COLOR_HIGHLIGHT);
            }

            char line[160];
            if (IsPathFavorited(app->path)) {
                snprintf(line, sizeof(line), " * %s", name);
                draw_text_colored_part(1, LIST_START_ROW + i, line, 3, COLOR_YELLOW);
            } else {
                snprintf(line, sizeof(line), "   %s", name);
                draw_text(1, LIST_START_ROW + i, line, COLOR_WHITE);
            }
        }

        draw_text(1, FOOTER_ROW, "A:Launch  B:Close  Y:Fav  -:Refresh  L:Top  R:Bottom  DPAD:Nav", COLOR_GRAY);

        if ((int)list->count > maxVisible) {
            int scrollMax = (int)list->count - maxVisible;
            float pct = (scrollMax > 0) ? (float)scrollOffset / scrollMax : 0;
            int barY = LIST_START_ROW + (int)(pct * (maxVisible - 2));
            draw_rect(s_cols - 1, barY, 2, 2, COLOR_GRAY);
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
                    rp = vol + 16; /* "/vol/external01/" */
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
