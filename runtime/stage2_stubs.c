// Temporary stubs for subsystems implemented in Phases F3-F6
// (Vulkan renderer, audio renderer, input, movie decoder, resolution scaler, frame pacing).

#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "ipc.h"

void frame_step_init(void) {}
int frame_step_realtime(void) { return 0; }
uint64_t frame_step_cap_ns(void) { return 16666667ull; }
double frame_step_limit_hz(void) { return 60.0; }
int64_t frame_step_world_us(void) { return -1; }

void res_init(void) {}
uint64_t res_heap_extra_bytes(void) { return 0; }
void prof_write(void) {}
void perf_main_thread(void) {}
void input_script_report(void) {}
void gfx_exit_release(void) {}
void gfx_shader_prepare_recorded(int a, int b) { (void)a; (void)b; }
int gfx_replay_async(void) { return 0; }
int gfx_window_hdr10(void) { return 0; }
void gfx_vsync_bind(uint64_t system_event) { (void)system_event; }
const GfxBackend gfx_vulkan_backend = {0};

void nvn_shutdown(void) {}
void nfp_shutdown(void) {}
void swkbd_shutdown(void) {}

void nvn_init(void) {
    rt_log("stage2: reached nvn_init (first NVN2 initialization)");
}
void nvn_procs_register(void) {}
uint64_t nvn_proc_address(const char *name) {
    rt_log("stage2: nvn_proc_address(%s)", name ? name : "(null)");
    return 0;
}
pool_t *nvn_pool_find(uint64_t addr, uint64_t *offset) {
    (void)addr;
    if (offset) *offset = 0;
    return NULL;
}

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

// Audio stubs
void audren_shutdown(void) {}
void ov_audio_shutdown(void) {}
int audren_renderers_live(void) { return 0; }
void audren_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s; (void)rq;
    rs->result = IPC_R_UNKNOWN_COMMAND;
}
void aud_user_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s; (void)rq;
    rs->result = IPC_R_UNKNOWN_COMMAND;
}

// Movie / Input stubs
void ov_movie_shutdown(void) {}
void ov_input_shutdown(void) {}
void input_shutdown(void) {}
void input_poll(void) {}
void input_motion_start(void) {}
int input_motion_enabled(void) { return 0; }
int input_rumble_triggers_enabled(void) { return 0; }
void input_rumble(float l, float r, float lt, float rt) { (void)l; (void)r; (void)lt; (void)rt; }
float rumble_motor(float amp, float freq) { (void)freq; return amp; }
int motion_global(void) { return 0; }
void motion_restart(void) {}
void motion_read(void *out) { (void)out; }
