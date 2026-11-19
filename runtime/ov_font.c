// Font engine work memory for the stand-in shared font.
//
// A game sizes each nn::fontll::ScalableFontEngine's work memory for Nintendo's fonts. MTF gives its
// second engine (the one its glyph thread uses) 50 KB; the host's stand-in font needs more for its tables, so
// FSS_load_font runs out of memory (error 201), the game ignores it, SetFont fails and every glyph lookup misses: the
// text draws as empty boxes. An engine with less than FONT_WORK_MIN gets that much guest memory of the host's instead;
// GetPointerToWorkBuffer still returns the game's buffer, which is what the game frees.
#include <stdint.h>

#include "host.h"
#include "platform/platform.h"

#define FONT_WORK_MIN 0x80000u   // what the same game's first engine gets, in which the font loads
#define FONT_ENGINES 16

static struct { uint64_t self, game, host; } work[FONT_ENGINES];
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

// uint32_t nn::fontll::ScalableFontEngine::LoadFont(char *name, const void *data, uint32_t index, uint32_t capacity):
// a game that hands the start of a shared-font BFTTF file
// (its 8-byte header, decoded to key 0 by ov_system.c) gets the sfnt after it; any other pointer goes on as it is.
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

HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj", obs_font_initialize);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine22GetPointerToWorkBufferEv", obs_font_work_buffer);
HOST_OBSERVER("_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj", obs_font_load);
