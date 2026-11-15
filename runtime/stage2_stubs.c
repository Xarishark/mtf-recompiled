// Temporary stubs for subsystems implemented in Phases F4-F6
// (Vulkan renderer, resolution scaler, DLC/AOC).

#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "ipc.h"

void res_init(void) {}
uint64_t res_heap_extra_bytes(void) { return 0; }
void gfx_shader_nvn2_dump(const uint8_t *ctl, const uint8_t *code) { (void)ctl; (void)code; }
int gfx_shader_is_nvn2(const uint8_t *ctl) { (void)ctl; return 0; }
int nvn2_lower(const uint8_t *ctl, const uint8_t *code, const uint8_t **nc, const uint8_t **nd) {
    (void)ctl; (void)code; (void)nc; (void)nd;
    return 0;
}
void gfx_exit_release(void) {}
void gfx_shader_prepare_recorded(int a, int b) { (void)a; (void)b; }
int gfx_replay_async(void) { return 0; }
int gfx_window_hdr10(void) { return 0; }
void gfx_vsync_bind(uint64_t system_event) { (void)system_event; }
double gfx_window_refresh_hz(void) { return 60.0; }
void gfx_window_toggle_fullscreen(void) {}
void *gfx_window_handle(void) { return NULL; }
void gfx_shader_counts(uint64_t *keys, uint64_t *translated) {
    if (keys) *keys = 0;
    if (translated) *translated = 0;
}
int menu_is_open(void) { return 0; }
void menu_toggle(void) {}
void menu_perf_cycle(void) {}
int flight_on = 0;
void flight_init(void) {}
void flight_frame(double a, int b) { (void)a; (void)b; }
void perfmon_frame(double a) { (void)a; }
const GfxBackend gfx_vulkan_backend = {0};

void nfp_shutdown(void) {}
void swkbd_shutdown(void) {}
void ov_audio_shutdown(void) {}
void ov_input_shutdown(void) {}

// Add-on content (DLC) stubs
void aoc_romfs_mount_all(void) {}
int aoc_romfs_lookup(const char *path, uint64_t *off, uint64_t *size) {
    (void)path; (void)off; (void)size;
    return 0;
}
int aoc_romfs_read(uint64_t off, void *dst, size_t len) {
    (void)off; (void)dst; (void)len;
    return -1;
}
int aoc_romfs_list_dir(const char *path, void *cb, void *ctx) {
    (void)path; (void)cb; (void)ctx;
    return 0;
}
int aoc_owns(const char *path) { (void)path; return 0; }
int aoc_handle(uint64_t h) { (void)h; return 0; }
uint32_t aoc_mount(const char *name) { (void)name; return FS_R_PATH_NOT_FOUND; }
void aoc_unmount(const char *name) { (void)name; }
uint32_t aoc_open(uint64_t *out, const char *path, int mode) {
    (void)out; (void)path; (void)mode;
    return FS_R_PATH_NOT_FOUND;
}
uint32_t aoc_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    (void)out; (void)h; (void)offset; (void)buffer; (void)size;
    return FS_R_INVALID;
}
uint32_t aoc_size(int64_t *out, uint64_t h) { (void)out; (void)h; return FS_R_INVALID; }
void aoc_close(uint64_t h) { (void)h; }
uint32_t aoc_entry_type(int *out, const char *path) { (void)out; (void)path; return FS_R_PATH_NOT_FOUND; }
int aoc_count(void) { return 0; }
int aoc_list(int *out, int offset, int count) { (void)out; (void)offset; (void)count; return 0; }
