// Output resolution and scene render scale (MTF_OUTPUT_RES / TOTK_OUTPUT_RES, MTF_RENDER_SCALE / TOTK_RENDER_SCALE).
//
// In Mario Tennis Fever (0400D4201C6EA000, v327680):
// 1. Display output resolution:
//    The game calls nn::vi::QueryDisplayBufferMaxResolution(int *w, int *h, Layer *) at nro+0x92a898, writes the
//    returned dimensions to its graphics device struct (+0x10/+0x14), and allocates its 2D UI and post-FXAA window
//    textures at that size (default 2560x1440 in docked mode, 1920x1080 in handheld mode).
//    ov_resolution_max_resolution overrides that query when MTF_OUTPUT_RES / TOTK_OUTPUT_RES is explicitly set by the
//    user (or != 1920x1080 default), and ov_resolution_layer patches the layer default at nro+0x929350.
// 2. Scene 3D render scale:
//    The game's dynamic resolution controller at nro+0x835020 reads two floats (scale X and scale Y, default 0.666667 =
//    2/3, giving 1706x960 for a 2560x1440 output) from a table at nro+0x14ac650 (4 entries of 8 bytes).
//    ov_resolution_apply writes the chosen scale (MTF_RENDER_SCALE / TOTK_RENDER_SCALE: "game" keeps 2/3, or 50..200%)
//    into that table and nop-patches the dynamic scaler clamp so the 3D scene renders at the requested fraction of the
//    output resolution.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "ov.h"

#define NRO_LAYER_W_ADD   (MOD_NRO0_BASE + 0x929354ull)   // add w8, w8, #0x780 (1920)
#define NRO_LAYER_H_MOV   (MOD_NRO0_BASE + 0x92935cull)   // mov w9, #0x438 (1080)
#define NRO_SCALE_TABLE   (MOD_NRO0_BASE + 0x14ac650ull)  // 4 pairs of (float sx, float sy)

static int parse_wh(const char *s, int *w, int *h) {
    if (!s || !*s) return 0;
    char *end = NULL;
    long pw = strtol(s, &end, 10);
    if (!end || (*end != 'x' && *end != 'X')) return 0;
    long ph = strtol(end + 1, &end, 10);
    if (!end || *end != 0) return 0;
    if (pw < 640 || pw > 7680 || ph < 360 || ph > 4320) return 0;
    *w = (int)pw, *h = (int)ph;
    return 1;
}

static void get_output_res(int *w, int *h) {
    *w = ov_handheld() ? 1920 : 2560;
    *h = ov_handheld() ? 1080 : 1440;
    if (!cfg_is_set("TOTK_OUTPUT_RES")) return;
    const char *v = cfg_get("TOTK_OUTPUT_RES");
    int pw, ph;
    if (v && parse_wh(v, &pw, &ph)) {
        *w = pw, *h = ph;
    }
}

static float get_render_scale(void) {
    const char *s = cfg_value("TOTK_RENDER_SCALE");
    if (!s || !*s || !strcmp(s, "game")) return 0.0f;
    char *end = NULL;
    double pct = strtod(s, &end);
    if (end == s || pct < 25.0 || pct > 200.0) return 0.0f;
    return (float)(pct / 100.0);
}

void ov_resolution_layer(void) {
    int w, h;
    get_output_res(&w, &h);
    // Patch the fallback mov/add in nro0 if nro0 is loaded
    if (MOD_NRO0_SIZE >= 0x929360ull) {
        uint32_t *iw = (uint32_t *)(uintptr_t)NRO_LAYER_W_ADD;
        uint32_t *ih = (uint32_t *)(uintptr_t)NRO_LAYER_H_MOV;
        if (*iw == 0x111e0108u && *ih == 0x52808709u && w <= 65535 && h <= 65535) {
            *iw = 0x52800008u | ((uint32_t)w << 5);   // mov w8, #w
            *ih = 0x52800009u | ((uint32_t)h << 5);   // mov w9, #h
        }
    }
}

void ov_resolution_apply(int *out_w, int *out_h) {
    int w, h;
    get_output_res(&w, &h);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    float sc = get_render_scale();
    if (sc > 0.0f && MOD_NRO0_SIZE >= 0x14ac670ull) {
        float *tab = (float *)(uintptr_t)NRO_SCALE_TABLE;
        for (int i = 0; i < 8; i++) tab[i] = sc;
    }
    static int logged;
    if (!logged++) {
        float eff = sc > 0.0f ? sc : (2.0f / 3.0f);
        rt_log("res: output %dx%d, 3D scene %dx%d (scale %.0f%%%s)",
               w, h, (int)(w * eff + 0.5f), (int)(h * eff + 0.5f), eff * 100.0f,
               sc > 0.0f ? "" : ", game default");
    }
}

int res_sizes(unsigned out[2], unsigned scene[2]) {
    int w, h;
    get_output_res(&w, &h);
    float sc = get_render_scale();
    float eff = sc > 0.0f ? sc : (2.0f / 3.0f);
    if (out) out[0] = (unsigned)w, out[1] = (unsigned)h;
    if (scene) scene[0] = (unsigned)(w * eff + 0.5f), scene[1] = (unsigned)(h * eff + 0.5f);
    return sc > 0.0f || cfg_is_set("TOTK_OUTPUT_RES");
}
