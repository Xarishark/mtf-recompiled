// Temporary stubs for subsystems implemented in Phases F4-F6
// (Vulkan renderer, resolution scaler, DLC/AOC).

#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "ipc.h"
#include "vk.h"
#include "vk_exec.h"

void res_init(void) {}
uint64_t res_heap_extra_bytes(void) { return 0; }
void res_note_upscaler(unsigned in_w, unsigned in_h, unsigned out_w, unsigned out_h) {
    (void)in_w; (void)in_h; (void)out_w; (void)out_h;
}
void res_fov_live(void) {}
int res_sizes(unsigned out[2], unsigned scene[2]) {
    if (out) out[0] = 2560, out[1] = 1440;
    if (scene) scene[0] = 2048, scene[1] = 1152;
    return 0;
}
int probe_census_dir(char *dir, size_t n) { (void)dir; (void)n; return 0; }
int nvn2_lower(const uint8_t *ctl, const uint8_t *code, const uint8_t **nc, const uint8_t **nd) {
    (void)ctl; (void)code; (void)nc; (void)nd;
    return 0;
}
uint64_t nvn2_origin(const uint8_t *code) { (void)code; return 0; }
int flight_on = 0;
void flight_init(void) {}
void flight_frame(double a, int b) { (void)a; (void)b; }

// RenderDoc & optional F6 Vulkan features (DLSS, FSR, SMAA, FG, HDR, USNAP, Headroom)
void rdoc_init(void) {}
int rdoc_active(void) { return 0; }
void rdoc_after_present(uint64_t frame) { (void)frame; }

const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void) { return NULL; }
void vk_headroom_start(void) {}
void vk_headroom_stop(void) {}

int vk_dlss_wanted(void) { return 0; }
int vk_dlss_instance_exts(const char **out, int max) { (void)out; (void)max; return 0; }
int vk_dlss_device_exts(const char **out, int max) { (void)out; (void)max; return 0; }
int vk_dlss_record(VkCommandBuffer cb, uint64_t fence, const VkTexImage *in, const VkTexImage *out) {
    (void)cb; (void)fence; (void)in; (void)out; return 0;
}
int vk_dlss_warm(VkCommandBuffer cb, uint64_t fence) { (void)cb; (void)fence; return 0; }
void vk_dlss_release(void) {}
void vk_dlss_scene_draw(const VkExec *ex) { (void)ex; }
void vk_dlss_jitter(VkImage depth, float j[2]) { (void)depth; j[0] = 0.0f; j[1] = 0.0f; }
void vk_dlss_part_begin(int k) { (void)k; }
void vk_dlss_part_taken(int k) { (void)k; }
void vk_dlss_image_gone(VkImage image) { (void)image; }

int vk_fg_record(VkImage src, VkFormat fmt, int w, int h, const int crop[4]) {
    (void)src; (void)fmt; (void)w; (void)h; (void)crop; return 0;
}
void vk_fg_show(int (*pending)(void), uint64_t frame) { (void)pending; (void)frame; }
void vk_fg_release(void) {}

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
