// Font engine work memory and dynamic Switch/Xbox/PlayStation/Keyboard button icons (nn::fontll::ScalableFontEngine).
//
// 1. Work memory: MTF gives its second font engine 50 KB; the host's stand-in font needs more, so small work buffers
//    are replaced with FONT_WORK_MIN of host-allocated guest memory.
// 2. Dynamic button icons: without firmware shared-fonts (5-nintendo-extension.bfttf), PUA codepoints (U+E0A0..U+E0FF)
//    render as '?'. We intercept CheckGlyphExist / AcquireGlyphmap / ReleasesGlyph for button PUA codepoints and
//    synthesize crisp 8-bit SDF icons (glyph_draw.c) matching MTF_BUTTON_ICONS or input_last_device().
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "glyph_draw.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

const unsigned char *menu_roboto_ttf(void);   // ui/menu.cpp

#define FONT_WORK_MIN 0x80000u
#define FONT_ENGINES 16

static struct { uint64_t self, game, host; int px; } work[FONT_ENGINES];
static PlatMutex lock = PLAT_MUTEX_INIT;

// void nn::fontll::ScalableFontEngine::Initialize(void *buffer, uint32_t size)
static void obs_font_initialize(Cpu *c) {
    const uint64_t self = c->x[0];
    const uint32_t size = (uint32_t)c->x[2];
    if (size < FONT_WORK_MIN) {
        plat_mutex_lock(&lock);
        int i = 0;
        while (i < FONT_ENGINES && work[i].self && work[i].self != self) i++;
        if (i < FONT_ENGINES) {
            if (!work[i].host && !(work[i].host = mem_host_alloc(FONT_WORK_MIN, 0x1000)))
                rt_fatal("font: no host memory for a font engine's work buffer");
            work[i].self = self;
            work[i].game = c->x[1];
            c->x[1] = work[i].host;
            c->x[2] = FONT_WORK_MIN;
            rt_log("font: engine %#llx: work buffer of %u bytes replaced by %u bytes of host memory",
                   (unsigned long long)self, size, FONT_WORK_MIN);
        } else {
            rt_log("[RED] font: more than %d font engines with a small work buffer: this one keeps the game's",
                   FONT_ENGINES);
        }
        plat_mutex_unlock(&lock);
    }
    rt_original(c);
}

// void *nn::fontll::ScalableFontEngine::GetPointerToWorkBuffer()
static void obs_font_work_buffer(Cpu *c) {
    const uint64_t self = c->x[0];
    uint64_t game = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < FONT_ENGINES && work[i].self; i++)
        if (work[i].self == self) game = work[i].game;
    plat_mutex_unlock(&lock);
    if (game) c->x[0] = game;
    else rt_original(c);
}

// uint32_t nn::fontll::ScalableFontEngine::LoadFont(char *name, const void *data, uint32_t index, uint32_t capacity)
#define BFTTF_MAGIC 0x7f9a0218u
static void obs_font_load(Cpu *c) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->x[2];
    if (p && mem_guest_readable(c->x[2], 12)) {
        uint32_t w0 = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        uint32_t w2 = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
        if (w0 == BFTTF_MAGIC && (w2 == 0x00010000u || w2 == 0x4f54544fu)) {
            static int logged;
            if (!logged++) rt_log("font: LoadFont got a BFTTF file start: given the font after its header");
            c->x[2] += 8;
        }
    }
    rt_original(c);
}

// void nn::fontll::ScalableFontEngine::SetScale(int32_t sx, int32_t sy, ...)
static volatile int g_last_px = 24;
static void obs_set_scale(Cpu *c) {
    int px = (int)((uint32_t)c->x[1] >> 16);
    if (px >= 8 && px <= GLYPH_CELL_MAX) __atomic_store_n(&g_last_px, px, __ATOMIC_RELAXED);
    rt_original(c);
}

// ---- Dynamic PUA button icon mapping & synthesis
enum { PK_NONE, PK_BTN, PK_DPAD };
static int map_pua(uint32_t cp, int *kind, int *arg) {
    switch (cp) {
    case 0xE0A0: case 0xE0E0: *kind = PK_BTN; *arg = B_A; return 1;
    case 0xE0A1: case 0xE0E1: *kind = PK_BTN; *arg = B_B; return 1;
    case 0xE0A2: case 0xE0E2: *kind = PK_BTN; *arg = B_X; return 1;
    case 0xE0A3: case 0xE0E3: *kind = PK_BTN; *arg = B_Y; return 1;
    case 0xE0A4: case 0xE0B5: case 0xE0E4: *kind = PK_BTN; *arg = B_L; return 1;
    case 0xE0A5: case 0xE0B6: case 0xE0E5: *kind = PK_BTN; *arg = B_R; return 1;
    case 0xE0A6: case 0xE0E6: *kind = PK_BTN; *arg = B_ZL; return 1;
    case 0xE0A7: case 0xE0E7: *kind = PK_BTN; *arg = B_ZR; return 1;
    case 0xE0C1: case 0xE0C4: *kind = PK_BTN; *arg = B_LS; return 1;
    case 0xE0C2: case 0xE0C5: *kind = PK_BTN; *arg = B_RS; return 1;
    case 0xE0EF: case 0xE0F1: *kind = PK_BTN; *arg = B_PLUS; return 1;
    case 0xE0F0: case 0xE0F2: *kind = PK_BTN; *arg = B_MINUS; return 1;
    case 0xE0AA: case 0xE0B0: case 0xE0EE: *kind = PK_DPAD; *arg = GLYPH_UP | GLYPH_DOWN | GLYPH_LEFT | GLYPH_RIGHT; return 1;
    case 0xE0AB: case 0xE0B1: case 0xE0EA: *kind = PK_DPAD; *arg = GLYPH_UP; return 1;
    case 0xE0AC: case 0xE0B2: case 0xE0EB: *kind = PK_DPAD; *arg = GLYPH_DOWN; return 1;
    case 0xE0AD: case 0xE0B3: case 0xE0EC: *kind = PK_DPAD; *arg = GLYPH_LEFT; return 1;
    case 0xE0AE: case 0xE0B4: case 0xE0ED: *kind = PK_DPAD; *arg = GLYPH_RIGHT; return 1;
    default: return 0;
    }
}

static int active_icon_device(void) {
    const char *v = cfg_value("MTF_BUTTON_ICONS");
    if (!strcmp(v, "nintendo")) return INPUT_DEV_NINTENDO;
    if (!strcmp(v, "xbox")) return INPUT_DEV_XBOX;
    if (!strcmp(v, "playstation")) return INPUT_DEV_PLAYSTATION;
    if (!strcmp(v, "keyboard")) return INPUT_DEV_KEYBOARD;
    return input_last_device();
}

#define POOL_SLOTS 64
#define SLOT_BYTES (64u + (size_t)GLYPH_CELL_MAX * GLYPH_CELL_MAX)
static uint64_t g_pool_base;
static uint32_t g_pool_next;
static PlatMutex g_glyph_lock = PLAT_MUTEX_INIT;

static void draw_button_icon(GlyphCanvas *c, int dev, int kind, int arg, int px) {
    float cx = px * 0.5f, cy = px * 0.5f, s = px * 0.86f;
    GlyphBox box = {cx, cy, s, s};
    if (kind == PK_DPAD) {
        if (dev == INPUT_DEV_KEYBOARD) glyph_keycap(c, box, NULL, arg);
        else glyph_dpad(c, box, arg);
        return;
    }
    if (dev == INPUT_DEV_KEYBOARD) {
        const char *lbl = "?";
        switch (arg) {
        case B_A: lbl = "K"; break;
        case B_B: lbl = "J"; break;
        case B_X: lbl = "I"; break;
        case B_Y: lbl = "U"; break;
        case B_L: lbl = "Q"; break;
        case B_R: lbl = "E"; break;
        case B_ZL: lbl = "Z"; break;
        case B_ZR: lbl = "C"; break;
        case B_PLUS: lbl = "Ent"; break;
        case B_MINUS: lbl = "Bk"; break;
        case B_LS: lbl = "WASD"; break;
        case B_RS: lbl = "Arr"; break;
        }
        glyph_keycap(c, box, lbl, 0);
        return;
    }
    if (dev == INPUT_DEV_PLAYSTATION) {
        switch (arg) {
        case B_A: glyph_ps_face(c, box, GLYPH_PS_CIRCLE); return;
        case B_B: glyph_ps_face(c, box, GLYPH_PS_CROSS); return;
        case B_X: glyph_ps_face(c, box, GLYPH_PS_TRIANGLE); return;
        case B_Y: glyph_ps_face(c, box, GLYPH_PS_SQUARE); return;
        case B_L: glyph_tab(c, box, "L1"); return;
        case B_R: glyph_tab(c, box, "R1"); return;
        case B_ZL: glyph_tab(c, box, "L2"); return;
        case B_ZR: glyph_tab(c, box, "R2"); return;
        case B_PLUS: glyph_menu_button(c, box, 1, 1); return;
        case B_MINUS: glyph_menu_button(c, box, 0, 1); return;
        case B_LS: glyph_round(c, box, "L3"); return;
        case B_RS: glyph_round(c, box, "R3"); return;
        }
    }
    if (dev == INPUT_DEV_XBOX) {
        const char *lbl = "?";
        switch (arg) {
        case B_A: lbl = "B"; break;
        case B_B: lbl = "A"; break;
        case B_X: lbl = "Y"; break;
        case B_Y: lbl = "X"; break;
        case B_L: glyph_tab(c, box, "LB"); return;
        case B_R: glyph_tab(c, box, "RB"); return;
        case B_ZL: glyph_tab(c, box, "LT"); return;
        case B_ZR: glyph_tab(c, box, "RT"); return;
        case B_PLUS: glyph_menu_button(c, box, 1, 0); return;
        case B_MINUS: glyph_menu_button(c, box, 0, 0); return;
        case B_LS: lbl = "LS"; break;
        case B_RS: lbl = "RS"; break;
        }
        glyph_round(c, box, lbl);
        return;
    }
    // INPUT_DEV_NINTENDO
    switch (arg) {
    case B_A: glyph_round(c, box, "A"); return;
    case B_B: glyph_round(c, box, "B"); return;
    case B_X: glyph_round(c, box, "X"); return;
    case B_Y: glyph_round(c, box, "Y"); return;
    case B_L: glyph_tab(c, box, "L"); return;
    case B_R: glyph_tab(c, box, "R"); return;
    case B_ZL: glyph_tab(c, box, "ZL"); return;
    case B_ZR: glyph_tab(c, box, "ZR"); return;
    case B_PLUS: glyph_round(c, box, "+"); return;
    case B_MINUS: glyph_round(c, box, "-"); return;
    case B_LS: glyph_round(c, box, "L"); return;
    case B_RS: glyph_round(c, box, "R"); return;
    }
}

static void obs_check_glyph(Cpu *c) {
    uint32_t cp = (uint32_t)c->x[1];
    int kind = 0, arg = 0;
    if (map_pua(cp, &kind, &arg)) {
        ret_x(c, 1);
        return;
    }
    rt_original(c);
}

static void obs_acquire_glyph(Cpu *c) {
    uint32_t cp = (uint32_t)c->x[1];
    int kind = 0, arg = 0;
    if (!map_pua(cp, &kind, &arg)) {
        rt_original(c);
        return;
    }
    plat_mutex_lock(&g_glyph_lock);
    static int font_init;
    if (!font_init) {
        font_init = 1;
        glyph_font_set(menu_roboto_ttf());
    }
    if (!g_pool_base) {
        g_pool_base = mem_host_alloc((uint64_t)POOL_SLOTS * SLOT_BYTES, 0x1000);
        if (!g_pool_base) rt_fatal("font: out of memory for button glyph pool");
    }
    uint32_t slot = g_pool_next++ % POOL_SLOTS;
    uint64_t hdr = g_pool_base + (uint64_t)slot * SLOT_BYTES;
    uint64_t bits_addr = hdr + 0x28;
    int px = __atomic_load_n(&g_last_px, __ATOMIC_RELAXED);
    if (px < 16) px = 24;
    if (px > GLYPH_CELL_MAX) px = GLYPH_CELL_MAX;
    static GlyphCanvas canvas;
    memset(&canvas, 0, sizeof canvas);
    canvas.size = px;
    draw_button_icon(&canvas, active_icon_device(), kind, arg, px);
    uint8_t *dst = (uint8_t *)(uintptr_t)bits_addr;
    for (int y = 0; y < px; y++) {
        for (int x = 0; x < px; x++) {
            int idx = y * GLYPH_CELL_MAX + x;
            float v = canvas.body[idx] * (1.0f - canvas.mark[idx] * 0.85f);
            int iv = (int)(v * 255.0f + 0.5f);
            dst[y * px + x] = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
        }
    }
    memset((void *)(uintptr_t)hdr, 0, 0x28);
    uint32_t *psize = (uint32_t *)(uintptr_t)(hdr + 8);
    int16_t *h = (int16_t *)(uintptr_t)hdr;
    *psize = (uint32_t)(0x28 + px * px);
    h[6] = 0;                        // loX
    h[7] = (int16_t)(px * 85 / 100); // hiY
    h[8] = (int16_t)(px + 2);        // dx
    h[11] = (int16_t)(px + 2);       // advance
    h[14] = (int16_t)px;             // width
    h[15] = (int16_t)px;             // height
    h[16] = (int16_t)px;             // bpl (bytes per line)
    h[18] = 8;                       // bitsPerPixel
    h[19] = (int16_t)c->x[2];        // mode
    plat_mutex_unlock(&g_glyph_lock);
    ret_x(c, hdr);
}

static void obs_release_glyph(Cpu *c) {
    uint64_t ptr = c->x[1];
    if (g_pool_base && ptr >= g_pool_base && ptr < g_pool_base + (uint64_t)POOL_SLOTS * SLOT_BYTES) return;
    rt_original(c);
}

HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj", obs_font_initialize);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine22GetPointerToWorkBufferEv", obs_font_work_buffer);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj", obs_font_load);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine8SetScaleEiiii", obs_set_scale);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine15CheckGlyphExistEj", obs_check_glyph);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt", obs_acquire_glyph);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine13ReleasesGlyphEPv", obs_release_glyph);
