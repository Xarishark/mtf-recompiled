// The game's resolution: its output (window textures and HUD) and its 3D scene.
//
// Output. The game computes its 2560x1440 once (nro f_700305ecc0: width, height, their halves, into globals and its
// renderer's settings object) and calls nn::vi::CreateLayer right after, before it reads any of them to make its window
// textures. MTF_OUTPUT_RES (720, 1080, 1440 or 2160 lines; auto: 2160 on a display that tall, else the game's 1440)
// replaces those values there, so the game's own code then makes every target at that size.
//
// Scene. The display-mode update that runs next (nro f_700305e8fc) asks vi for the largest display buffer, caps the
// mode size it holds in x20 at that buffer, scales it by a global mode int (0 the whole output, 1 80 %: what it uses
// docked, 2 two thirds) and sizes the scene targets from the result. At the query the output size goes into that
// structure, and the scene's share (MTF_RENDER_SCALE in percent, or a DLSS upscaling mode, vk_dlaa.c) scales it there
// with the mode int set to 0, so the game takes any size as it takes its own. The game does this once, at start.
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

int vk_dlaa_scene_share(void);   // vk_dlaa.c

static int scene_w, scene_h, out_w, out_h;   // ov_resolution_apply's decision

#define SCALE_MODE_PTR 0x7003560ef0ull        // the game's pointer to that int
#define CREATE_LAYER_RETURN 0x700305f258ull   // f_700305ecc0's call of nn::vi::CreateLayer
#define LAYER_TO_SETTINGS 0xe8                // CreateLayer's first argument is this far into the settings object
#define SETTINGS_SIZE 0x108                   // width and height in that object
#define WIDTH_PTR 0x700355ede8ull             // the game's pointers to its output width, height and their halves
#define HEIGHT_PTR 0x700355edf0ull
#define HALF_WIDTH_PTR 0x7003560a30ull
#define HALF_HEIGHT_PTR 0x7003560468ull
#define MODE_SIZE 0x1c                        // the display-mode structure's width and height, two 16-bit values

// The display's height in physical pixels (DPI scaling aside), 0 when unknown.
static int display_lines(void) {
#ifdef _WIN32
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    return EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmPelsHeight : 0;
#else
    return 0;
#endif
}

// The output's lines: MTF_OUTPUT_RES, or with auto the window/display height (matching 720..4320) instead of forcing
// 1440p on a 1080p window.
int ov_resolution_lines(void) {
    static int lines;
    if (lines) return lines;
    const char *v = cfg_value("MTF_OUTPUT_RES");
    int r = atoi(v);
    if (!strcmp(v, "auto") || r <= 0) {
        int d = display_lines();
        if (!cfg_bool("MTF_FULLSCREEN")) {
            int wx = 0, wy = 0;
            if (sscanf(cfg_value("MTF_WINDOW_SIZE"), "%dx%d", &wx, &wy) == 2 && wy >= 360 && (!d || wy < d)) d = wy;
        }
        lines = d >= 4320 ? 4320 : d >= 2880 ? 2880 : d >= 2160 ? 2160 : d >= 1440 ? 1440 : d >= 1080 ? 1080 : 720;
        rt_log("resolution: output %d lines (auto: target %d)", lines, d);
    } else {
        lines = r < 480 ? 480 : r > 4320 ? 4320 : (r & ~1);
    }
    return lines;
}

// The output size when it is not the game's own 2560x1440.
static int output_size(int *w, int *h) {
    int r = ov_resolution_lines();
    if (r == 1440) {
        *w = *h = 0;
        return 0;
    }
    *h = r;
    *w = r == 4320 ? 7680 : r == 2880 ? 5120 : r == 2160 ? 3840 : r == 1080 ? 1920 : r == 720 ? 1280
         : ((r * 16 + 8) / 9 + 1) & ~1;
    return 1;
}

static void write_through(uint64_t pointer, int32_t value) {
    if (!mem_guest_readable(pointer, 8)) return;
    uint64_t target = *(const uint64_t *)(uintptr_t)pointer;
    if (target && mem_guest_readable(target, 4)) *(int32_t *)(uintptr_t)target = value;
}

// From nn::vi::CreateLayer (ov_gfx.c).
void ov_resolution_layer(Cpu *c) {
    int w, h;
    if (c->x[30] != CREATE_LAYER_RETURN || !output_size(&w, &h)) return;
    uint64_t settings = c->x[0] - LAYER_TO_SETTINGS;
    if (!mem_guest_readable(settings + SETTINGS_SIZE, 8)) return;
    int32_t *size = (int32_t *)(uintptr_t)(settings + SETTINGS_SIZE);
    rt_log("resolution: output %dx%d (the game's is %dx%d)", w, h, size[0], size[1]);
    size[0] = w, size[1] = h;
    write_through(WIDTH_PTR, w);
    write_through(HEIGHT_PTR, h);
    write_through(HALF_WIDTH_PTR, w / 2);
    write_through(HALF_HEIGHT_PTR, h / 2);
}

// The scene's share of the output in thousandths: a DLSS upscaling mode (MTF_DLSS, while DLAA runs: vk_dlaa.c
// vk_dlaa_scene_share), else MTF_RENDER_SCALE in percent; 0 keeps the game's own choice (80 % docked).
static int scene_share(void) {
    int k = vk_dlaa_scene_share();
    if (k) return k;
    const char *v = cfg_value("MTF_RENDER_SCALE");
    if (!strcmp(v, "game")) return 0;
    int pct = atoi(v);
    return pct <= 0 ? 0 : pct > 200 ? 2000 : pct * 10;
}

// From nn::vi::QueryDisplayBufferMaxResolution (ov_net.c): right after this call the game caps the mode size in x20 at
// the display's largest buffer, scales it by its mode (100, 80 or 67 %) and sizes its scene targets from the result
// (f_700305e8fc). The output size goes into the structure here, and with a share of our own the scene size too, with
// the game's mode set to 100 % so it takes that size as it is.
void ov_resolution_apply(Cpu *c) {
    uint64_t mode_size = c->x[20] + MODE_SIZE;
    if (!mem_guest_readable(mode_size, 4)) return;
    uint16_t *size = (uint16_t *)(uintptr_t)mode_size;
    if (!size[0] || !size[1]) return;
    int w, h;
    if (output_size(&w, &h)) size[0] = (uint16_t)w, size[1] = (uint16_t)h;
    out_w = size[0], out_h = size[1];
    int k = scene_share();
    if (!k || !mem_guest_readable(SCALE_MODE_PTR, 8)) return;
    uint64_t ptr = *(const uint64_t *)(uintptr_t)SCALE_MODE_PTR;
    if (!ptr || !mem_guest_readable(ptr, 4)) return;
    uint32_t *mode = (uint32_t *)(uintptr_t)ptr;
    int ow = size[0], oh = size[1];
    int sw = (ow * k / 1000 + 1) & ~1, sh = (oh * k / 1000 + 1) & ~1;   // even sizes, as the game's own modes give
    if (sw > 7680) sw = 7680;
    if (sh > 4320) sh = 4320;
    size[0] = (uint16_t)sw, size[1] = (uint16_t)sh;
    static int logged;
    if (!logged++) rt_log("resolution: scene %dx%d of the output %dx%d (the game asked for mode %u)", sw, sh, ow, oh, *mode);
    *mode = 0;
    scene_w = sw, scene_h = sh;
}

// The sizes decided above, for DLSS (vk_dlaa.c res_sizes); 0 before the game asked.
void ov_resolution_sizes(unsigned out[2], unsigned scene[2]) {
    out[0] = (unsigned)out_w, out[1] = (unsigned)out_h, scene[0] = (unsigned)scene_w, scene[1] = (unsigned)scene_h;
}
