// Hooks of presentation extras this port does not have (HDR output, per-object motion vectors, contact
// shadows, GPU crumbs, the profiler) and small shared pieces of the present path. The back end calls the hooks at fixed
// points; each answers what the feature switched off gives, so the present path takes its plain route.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk.h"
#include "vk_dlaa.h"

// ---- per-object motion vectors, contact shadows and GPU crumbs: off
void vk_mvprev_frame(void) {}
void vk_mvsurvey_frame(void) {}
void vk_contact_release(void) {}
void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) { (void)cb, (void)kind, (void)n; }

#ifdef _WIN32
#include <windows.h>   // (VirtualQuery below)
#endif

// ---- camera field of view, resolution switch and play time: not in this game
void res_fov_live(void) {}
void res_present(Cpu *c) { (void)c; }
void playtime_save(void) {}

// ---- frame pacing: the game's own present interval paces it
int frame_step_realtime(void) { return 0; }
uint64_t frame_step_cap_ns(void) { return 0; }
void frame_step_present(uint64_t display_ns, int interval) { (void)display_ns, (void)interval; }

// ---- performance counters: the render thread's waits per frame, the event counts and the GPU's busy time, taken while
// the full performance overlay shows them (perf_set_overlay) or MTF_TRACE=perf asks. Off, perf_begin answers 0, perf_end
// and perf_replay return at once and perf_count is a load and a branch.
static int perf_overlay, perf_on;   // the overlay's mode (0 off, 1 fps, 2 full); wait times and counts are taken
static volatile uint32_t perf_render_tid, perf_replay_tid;
static uint64_t perf_frame_ns[PERF_N];   // this frame's waits of the render or replay thread
static volatile int64_t perf_frame_replay_ns, perf_counts[PERF_CNT_N], perf_gpu_total;

static int perf_wanted(void) { return (rt_trace & RT_TRACE_PERF) != 0 || __atomic_load_n(&perf_overlay, __ATOMIC_RELAXED) == 2; }

void perf_set_overlay(int mode) {
    __atomic_store_n(&perf_overlay, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_on, perf_wanted(), __ATOMIC_RELAXED);
}

uint64_t perf_begin(void) { return __atomic_load_n(&perf_on, __ATOMIC_RELAXED) ? kern_now_ns() : 0; }

void perf_end(int kind, uint64_t t0) {
    if (!t0) return;
    uint32_t tid = plat_thread_id();
    if (tid != perf_render_tid && tid != perf_replay_tid) return;
    __atomic_fetch_add(&perf_frame_ns[kind], kern_now_ns() - t0, __ATOMIC_RELAXED);
}

void perf_count(int kind) {
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) plat_inc64(&perf_counts[kind]);
}

void perf_replay(uint64_t t0) {
    if (t0) plat_add64(&perf_frame_replay_ns, (int64_t)(kern_now_ns() - t0));
}

int perf_gpu_timing(void) { return perf_wanted(); }
void perf_gpu_busy(uint64_t ns) { plat_add64(&perf_gpu_total, (int64_t)ns); }
uint64_t perf_gpu_busy_total(void) { return (uint64_t)__atomic_load_n(&perf_gpu_total, __ATOMIC_RELAXED); }
void gfx_window_fps(double fps, double frame_ms);   // gfx_window_win.c

// MTF_SCAN_DT=<frame> (diagnosis): at that
// present and 300 later, counts the floats of the frame steps (1/60, 1/30 and those between 0.015 and 0.04) in the
// process's private read-write memory and writes their addresses to dt-<frame>.txt, to find the game's frame step.
static void scan_dt(uint64_t frame) {
    static uint64_t at = (uint64_t)-1;
    if (at == (uint64_t)-1) {
        const char *e = cfg_get("MTF_SCAN_DT");
        at = e && *e ? strtoull(e, NULL, 10) : 0;
    }
    if (!at || (frame != at && frame != at + 300)) return;
    char name[64];
    snprintf(name, sizeof name, "dt-%llu.txt", (unsigned long long)frame);
    FILE *f = fopen(name, "w");
    if (!f) return;
    uint64_t n60 = 0, n30 = 0, nrange = 0;
    MEMORY_BASIC_INFORMATION m;
    for (uint8_t *a = (uint8_t *)0x10000; VirtualQuery(a, &m, sizeof m); a = (uint8_t *)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE || m.Protect != PAGE_READWRITE) continue;
        const uint32_t *w = m.BaseAddress;
        for (size_t i = 0; i < m.RegionSize / 4; i++) {
            uint32_t v = w[i];
            if (v < 0x3c75c28f || v > 0x3d23d70a) continue;   // 0.015 .. 0.04
            nrange++;
            if (v == 0x3c888889) n60++;
            if (v == 0x3d088889) n30++;
            if (nrange <= 200000) fprintf(f, "%p %08x\n", (void *)&w[i], v);
        }
    }
    fclose(f);
    rt_log("scan_dt: frame %llu: %llu floats in 0.015..0.04, %llu of 1/60, %llu of 1/30 (%s)", (unsigned long long)frame,
           (unsigned long long)nrange, (unsigned long long)n60, (unsigned long long)n30, name);
}

// The per-thread time split (MTF_TRACE=sched); its overlay is not ported. Once a second the frame rate
// goes to the window title and, with MTF_FPS_LOG=1, to the log.
void perf_present(uint64_t frame) {
    if (rt_trace & RT_TRACE_SCHED) kern_sched_report(frame);
    scan_dt(frame);
    static uint64_t since, frames, last;
    uint64_t now = kern_now_ns();
    perf_render_tid = plat_thread_id();
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED) != perf_wanted())
        __atomic_store_n(&perf_on, perf_wanted(), __ATOMIC_RELAXED);   // MTF_TRACE=perf: on from the first present
    if (__atomic_load_n(&perf_overlay, __ATOMIC_RELAXED))              // the overlay's only cost while it is off
        perfmon_frame(now, last ? now - last : 0, perf_frame_ns,
                      (uint64_t)__atomic_exchange_n(&perf_frame_replay_ns, 0, __ATOMIC_RELAXED),
                      __atomic_load_n(&perf_counts[PERF_CNT_TEX_UP], __ATOMIC_RELAXED));
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) memset(perf_frame_ns, 0, sizeof perf_frame_ns);
    last = now;
    if (!since) since = now;
    frames++;
    if (now - since >= 1000000000ull) {
        double s = (now - since) / 1e9, fps = frames / s;
        gfx_window_fps(fps, 1000.0 / fps);
        static int log = -1;
        if (log < 0) log = cfg_bool("MTF_FPS_LOG");
        if (log) rt_log("fps: %.1f (frame %llu)", fps, (unsigned long long)frame);
        since = now, frames = 0;
    }
}
void perf_replay_thread(void) { perf_replay_tid = plat_thread_id(); }
void prof_write(void) {}

// MTF_<name>: 0 off, 2 for "ab" (the A/B switch, off here: rt_ab is 0), 1 for anything else; def when unset (perf.c).
int rt_knob(const char *name, const char *def) {
    char key[64];
    snprintf(key, sizeof key, "MTF_%s", name);
    const char *e = cfg_get(key);
    if (!e || !*e) e = def;
    return !strcmp(e, "ab") ? 2 : strcmp(e, "0") != 0;
}
int rt_ab(void) { return 0; }

// ---- movies (ov_movie.c): not ported yet
void movie_tick(Cpu *c) { (void)c; }

// ---- a frame reached the screen
// MTF_TRACE=perf: the game's present intervals, logged every 600 presents: the average, the worst, and how many took
// longer than 17.5, 20 and 25 ms (at 60 fps every one should take 16.7). Called on the game's presenting thread.
void rt_frame_presented(const tex_t *t, uint64_t n) {
    (void)t;
    if (n == 120) nvn_procs_scan();   // the game has loaded its NVN procs by then
    if (!(rt_trace & RT_TRACE_PERF)) return;
    static uint64_t last, sum, worst, count, over[3];
    uint64_t now = kern_now_ns();
    if (last) {
        uint64_t d = now - last;
        sum += d, count++;
        if (d > worst) worst = d;
        over[0] += d > 17500000, over[1] += d > 20000000, over[2] += d > 25000000;
        if (count == 600) {
            rt_log("perf: frames %.2f ms average, worst %.1f ms, over 17.5 ms %llu, over 20 ms %llu, over 25 ms %llu (of 600)",
                   sum / 600 / 1e6, worst / 1e6, (unsigned long long)over[0], (unsigned long long)over[1],
                   (unsigned long long)over[2]);
            if (perf_gpu_stats) {
                PerfGpu g;
                memset(&g, 0, sizeof g);
                perf_gpu_stats(&g);
                if (g.have_gpu)
                    rt_log("perf: GPU busy %.2f ms a frame in %.1f parts (%llu parts not timed)", g.gpu_ns / 600 / 1e6,
                           g.gpu_parts / 600.0, (unsigned long long)g.gpu_missed);
            }
            sum = worst = count = over[0] = over[1] = over[2] = 0;
        }
    }
    last = now;
}

// ---- PNG writer for screenshots (MTF_SHOTS): RGBA, uncompressed (stored deflate blocks), CRC-32 and Adler-32
static uint32_t png_crc(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
static void png_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t b[8];
    png_be32(b, n);
    memcpy(b + 4, type, 4);
    fwrite(b, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = png_crc(png_crc(0, (const uint8_t *)type, 4), data, n);
    png_be32(b, crc);
    fwrite(b, 1, 4, f);
}
void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    size_t row = (size_t)w * 4 + 1, raw = row * h, blocks = (raw + 65534) / 65535;
    size_t zn = 2 + raw + blocks * 5 + 4;
    uint8_t *z = malloc(zn), *img = malloc(raw);
    FILE *f = z && img ? fopen(path, "wb") : NULL;
    if (!f) {
        free(z), free(img);
        rt_log("png: %s could not be written", path);
        return;
    }
    for (uint32_t y = 0; y < h; y++) img[y * row] = 0, memcpy(img + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    size_t k = 0;
    z[k++] = 0x78, z[k++] = 0x01;
    uint32_t a = 1, b2 = 0;
    for (size_t i = 0; i < raw; i++) a = (a + img[i]) % 65521, b2 = (b2 + a) % 65521;
    for (size_t off = 0; off < raw; off += 65535) {
        uint16_t len = (uint16_t)(raw - off < 65535 ? raw - off : 65535);
        z[k++] = off + len >= raw;
        z[k++] = (uint8_t)len, z[k++] = (uint8_t)(len >> 8), z[k++] = (uint8_t)~len, z[k++] = (uint8_t)(~len >> 8);
        memcpy(z + k, img + off, len);
        k += len;
    }
    png_be32(z + k, b2 << 16 | a);
    k += 4;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    png_be32(ihdr, w), png_be32(ihdr + 4, h);
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)k);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z), free(img);
}

// ---- debugging and analysis hooks of the back end, all off: RenderDoc, flight recorder, frame dumps, GPU crumbs, pass marks,
// VRAM headroom monitor, DLAA motion vectors, contact shadows, the HDR tonemap draw. Each answers "off" with the
// signature its header declares.
#include "flight.h"
#include "vk_exec.h"
int flight_on;   // flight.h
int rdoc_active(void) {  return 0; }   // vk.h
void rdoc_after_present(uint64_t frame) { (void)frame; }   // vk.h
void rdoc_init(void) {  }   // vk.h
void vk_contact_flush(VkExec *ex) { (void)ex; }   // vk_exec.h
void vk_contact_pass_begin(VkExec *ex) { (void)ex; }   // vk_exec.h
void vk_crumbs_init(VkCtx *c) { (void)c; }   // vk.h
void vk_crumbs_release(void) {  }   // vk.h
void vk_crumbs_report(void) {  }   // vk.h
const VmaDeviceMemoryCallbacks * vk_headroom_callbacks(void) {  return NULL; }   // vk.h
void vk_headroom_start(void) {  }   // vk.h
void vk_headroom_stop(void) {  }   // vk.h
void vk_mvprev_draw(VkExec *ex) { (void)ex; }   // vk_dlaa.h
void vk_mvsurvey_draw(VkExec *ex) { (void)ex; }   // vk_dlaa.h
VkPassMark * vk_pass_mark_begin(VkCommandBuffer cb, int kind, uint64_t fence) { (void)cb, (void)kind, (void)fence; return NULL; }   // vk.h
void vk_pass_mark_end(VkCommandBuffer cb, const VkPassMark *m) { (void)cb, (void)m; }   // vk.h
void vk_pass_marks_report(void) {  }   // vk.h
int vk_pass_marks_want_nv(void) {  return 0; }   // vk.h
void perf_gpu_wait(uint64_t t0) { (void)t0; }   // host.h
// host.h
int vk_contact_strength(void) {  return 0; }   // vk.h
uint64_t vk_mvprev_step(int stage, int index, uint64_t size) { (void)stage, (void)index, (void)size; return 0; }   // vk.h
void vk_pass_mark_cmd(VkCommandBuffer cb, uint32_t id, int end) { (void)cb, (void)id, (void)end; }   // vk.h
void (*perf_gpu_stats)(PerfGpu *out);   // host.h: no GPU statistics consumer
