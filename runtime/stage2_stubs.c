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
int probe_census_dir(char *dir, size_t n) { (void)dir; (void)n; return 0; }
int nvn2_lower(const uint8_t *ctl, const uint8_t *code, const uint8_t **nc, const uint8_t **nd) {
    (void)ctl; (void)code; (void)nc; (void)nd;
    return 0;
}
uint64_t nvn2_origin(const uint8_t *code) { (void)code; return 0; }
int menu_is_open(void) { return 0; }
void menu_toggle(void) {}
void menu_perf_cycle(void) {}
int menu_window_msg(void *h, unsigned m, uintptr_t w, intptr_t l, intptr_t *res) {
    (void)h; (void)m; (void)w; (void)l; (void)res; return 0;
}
int menu_overlay_frame(uint64_t frame, const void *mt, const char **shot) {
    (void)frame; (void)mt; if (shot) *shot = NULL; return 0;
}
void menu_overlay_draw(VkCommandBuffer cb) { (void)cb; }
void menu_vk_release(void) {}
int flight_on = 0;
void flight_init(void) {}
void flight_frame(double a, int b) { (void)a; (void)b; }
void perfmon_frame(double a) { (void)a; }

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

int vk_fsr_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, int sw, int sh, const int crop[4], VkImage dst, const int to[4]) {
    (void)cb; (void)slot; (void)src; (void)fmt; (void)sw; (void)sh; (void)crop; (void)dst; (void)to; return 0;
}
void vk_fsr_release(void) {}

int vk_smaa_record(VkCommandBuffer cb, VkImage dst, const int to[4]) { (void)cb; (void)dst; (void)to; return 0; }
void vk_smaa_release(void) {}

int vk_fg_record(VkImage src, VkFormat fmt, int w, int h, const int crop[4]) {
    (void)src; (void)fmt; (void)w; (void)h; (void)crop; return 0;
}
void vk_fg_show(int (*pending)(void), uint64_t frame) { (void)pending; (void)frame; }
void vk_fg_release(void) {}

int vk_hdr_wanted(void) { return 0; }
void vk_hdr_off(void) {}
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    (void)f; (void)n; (void)out; return 0;
}
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) { (void)sc; (void)new_sc; }
VkImage vk_hdr_mid(VkCommandBuffer cb, uint32_t w, uint32_t h) { (void)cb; (void)w; (void)h; return VK_NULL_HANDLE; }
int vk_hdr_record(VkCommandBuffer cb, VkImage dst, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    (void)cb; (void)dst; (void)dst_view; (void)fmt; (void)w; (void)h; return 0;
}
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) { (void)cb; (void)w; (void)h; return VK_NULL_HANDLE; }
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    (void)cb; (void)dst_view; (void)fmt; (void)w; (void)h;
}
void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) {
    (void)in; (void)out; (void)px; (void)a2r;
}
void vk_hdr_release(void) {}

void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence) { (void)handles; (void)n; (void)fence; }
void vk_usnap_end(uint64_t fence) { (void)fence; }
int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    (void)addr; (void)n; (void)align; (void)out; return 0;
}
void vk_usnap_quiet(int on) { (void)on; }
uint64_t vk_usnap_drops(void) { return 0; }
void vk_usnap_view(uint64_t upto) { (void)upto; }
const void *vk_usnap_bytes(uint64_t addr, uint64_t n) { (void)addr; (void)n; return NULL; }
int vk_usnap_covers(uint64_t addr, uint64_t n) { (void)addr; (void)n; return 0; }
void vk_usnap_drop(uint64_t addr, uint64_t n) { (void)addr; (void)n; }
uint64_t vk_usnap_arena_bytes(void) { return 0; }
uint64_t vk_usnap_gen(void) { return 0; }
void vk_usnap_shutdown(void) {}

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
