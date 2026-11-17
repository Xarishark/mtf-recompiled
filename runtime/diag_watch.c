// MTF_WATCH=<addr>[+<len>] (hex guest address, len in bytes: default 8, at most 32): which code writes a block of guest
// memory, and when, against the commands that read it. MTF_WATCH=c<n> watches the first 32 bytes of the first uniform
// buffer bound at hardware bank n (NVN binding n - 3, as the frame dump names banks), for a block whose address
// changes between runs. Hardware write watchpoints (platform.h plat_hw_watch) cover the
// range in 8-byte aligned slots, and the log gets a timeline whose lines start with the frame being built and the
// number of QueueSubmitCommands calls so far:
//   write   the slot's value after the store, and the functions on the writing thread's stack, innermost first
//           (guest addresses of lifted functions; "host" for runtime code)
//   bind    a uniform or storage buffer bind whose range reaches the block, and the command buffer it went into
//   update  UpdateUniformBuffer or CopyBufferToBuffer writing into the block
//   end     EndRecording of a command buffer that recorded one of those, with the handle it returned
//   submit  QueueSubmitCommands, with the marked handles among those submitted
//   wait    nvnSyncWait
// Window: MTF_WATCH_KEY=1 logs the MTF_WATCH_FRAMES frames (default 2) after each F12 in the game window, otherwise
// every frame from MTF_WATCH_FROM (default 0) on. MTF_WATCH_LOG caps the lines of the run (default 4000).
#include "diag_watch.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

int diag_watch_on;

static struct {
    uint64_t base, end;          // the watched bytes, rounded out to the slots
    int key_mode, frames;
    uint64_t from, lo, hi;       // window: frames [lo, hi] (key mode) or from on
    int32_t lines, cap;
    int pending;                 // MTF_WATCH=c<n>: the NVN uniform binding that arms the watch, else -1
    PlatMutex lock;              // marked command buffers
    const void *marked[64];
    int nmarked;
} W = {.pending = -1, .lock = PLAT_MUTEX_INIT};

static uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1; }

static int in_window(void) {
    uint64_t f = frame_now();
    if (!W.key_mode) return f >= W.from;
    return f >= __atomic_load_n(&W.lo, __ATOMIC_RELAXED) && f <= __atomic_load_n(&W.hi, __ATOMIC_RELAXED);
}

// A line may be written: inside the window and under the cap.
static int take_line(void) {
    if (!in_window()) return 0;
    int32_t n = plat_inc32(&W.lines);
    if (n == W.cap) rt_log("watch: %d lines written, the rest of the run is not logged (MTF_WATCH_LOG)", W.cap);
    return n < W.cap;
}

static void vnote(const char *fmt, va_list ap) {
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    rt_log("watch: f%llu s%llu %s", (unsigned long long)frame_now(), (unsigned long long)nvn_submit_count(), msg);
}

void diag_watch_note(const char *fmt, ...) {
    if (!diag_watch_on || !take_line()) return;
    va_list ap;
    va_start(ap, fmt);
    vnote(fmt, ap);
    va_end(ap);
}

int diag_watch_hits(uint64_t addr, uint64_t size) {
    uint64_t base = __atomic_load_n(&W.base, __ATOMIC_ACQUIRE), end = __atomic_load_n(&W.end, __ATOMIC_RELAXED);
    return base && addr < end && addr + (size ? size : 1) > base;
}

static void log_armed(void);

// Runs in the exception handler of the writing thread, right after the store: formats and logs, waits on nothing the
// game code can hold (the log lock is taken only by the runtime's own logging).
static void on_write(int slot, uintptr_t rip, const uintptr_t *starts, int n) {
    if (!take_line()) return;
    uint64_t a = W.base + 8 * (uint64_t)slot, v;
    memcpy(&v, (const void *)(uintptr_t)a, sizeof v);
    float f[2];
    memcpy(f, &v, sizeof f);
    char chain[480];
    size_t k = 0;
    int host_run = 0;
    for (int i = 0; i < n && k + 48 < sizeof chain; i++) {
        uint64_t g = rt_lifted_function(starts[i]);
        if (!g && host_run) continue;   // consecutive runtime frames as one "host"
        host_run = !g;
        int w = g ? snprintf(chain + k, sizeof chain - k, "%s%#llx", k ? " < " : "", (unsigned long long)g)
                  : snprintf(chain + k, sizeof chain - k, "%shost", k ? " < " : "");
        if (i == 0 && w > 0 && k + (size_t)w + 24 < sizeof chain) {
            k += (size_t)w;
            w = snprintf(chain + k, sizeof chain - k, " (+%#llx)", (unsigned long long)(rip - starts[0]));
        }
        if (w > 0) k += (size_t)w;
    }
    chain[k] = 0;
    char msg[640];
    snprintf(msg, sizeof msg, "write %#llx = %016llx (%g %g) by %s", (unsigned long long)a, (unsigned long long)v,
             (double)f[0], (double)f[1], k ? chain : "?");
    rt_log("watch: f%llu s%llu %s", (unsigned long long)frame_now(), (unsigned long long)nvn_submit_count(), msg);
}

// Watches [addr, addr + len): 8-byte slots, 4 at most.
static int arm(uint64_t addr, uint64_t len) {
    uint64_t base = addr & ~7ull, end = (addr + len + 7) & ~7ull;
    int slots = (int)((end - base) / 8);
    if (slots > 4) {
        rt_log("watch: the range covers %d slots, the first 4 are watched (32 bytes)", slots);
        slots = 4, end = base + 32;
    }
    uint64_t at[4];
    int lens[4];
    for (int i = 0; i < slots; i++) at[i] = base + 8 * (uint64_t)i, lens[i] = 8;
    if (!plat_hw_watch(at, lens, slots, on_write)) {
        rt_log("[RED] watch: the hardware watchpoints could not be set up");
        return 0;
    }
    __atomic_store_n(&W.end, end, __ATOMIC_RELAXED);
    __atomic_store_n(&W.base, base, __ATOMIC_RELEASE);
    return 1;
}

void diag_watch_uniform(int index, uint64_t addr, uint64_t size) {
    static volatile int32_t armed;
    if (index != W.pending || !addr || plat_xchg32(&armed, 1)) return;
    rt_log("watch: c%d first bound at %#llx+%#llx", index + 3, (unsigned long long)addr, (unsigned long long)size);
    if (arm(addr, size && size < 32 ? size : 32)) log_armed();
}

void diag_watch_init(void) {
    const char *e = cfg_get("MTF_WATCH");
    if (!e || !*e) return;
    W.key_mode = cfg_bool("MTF_WATCH_KEY");
    W.frames = cfg_get("MTF_WATCH_FRAMES") ? atoi(cfg_get("MTF_WATCH_FRAMES")) : 2;
    W.from = cfg_get("MTF_WATCH_FROM") ? strtoull(cfg_get("MTF_WATCH_FROM"), NULL, 0) : 0;
    W.cap = cfg_get("MTF_WATCH_LOG") ? atoi(cfg_get("MTF_WATCH_LOG")) : 4000;
    if (W.frames < 1) W.frames = 1;
    W.lo = W.hi = 0;
    rt_lifted_function(0);   // builds the host-to-guest table on this thread, before any write can look it up
    char *end;
    if (e[0] == 'c' && e[1] >= '0' && e[1] <= '9') {
        long bank = strtol(e + 1, &end, 10);
        if (*end || bank < 3 || bank >= 3 + 18) {
            rt_log("[RED] watch: MTF_WATCH=%s: uniform banks are c3..c20", e);
            return;
        }
        W.pending = (int)bank - 3;
        diag_watch_on = 1;
        rt_log("watch: waiting for the first uniform buffer bound at c%ld", bank);
        return;
    }
    uint64_t addr = strtoull(e, &end, 16), len = 8;
    if (*end == '+') len = strtoull(end + 1, &end, 0);
    if (!addr || !len || *end) {
        rt_log("[RED] watch: MTF_WATCH=%s: expected <hex address>[+<length>] or c<bank>", e);
        return;
    }
    if (!arm(addr, len)) return;
    diag_watch_on = 1;
    log_armed();
}

static void log_armed(void) {
    if (W.key_mode)
        rt_log("watch: writes to %#llx..%#llx; each F12 in the game window logs the next %d frame(s)",
               (unsigned long long)W.base, (unsigned long long)W.end, W.frames);
    else
        rt_log("watch: writes to %#llx..%#llx from frame %llu", (unsigned long long)W.base, (unsigned long long)W.end,
               (unsigned long long)W.from);
}

void diag_watch_key(void) {
    if (!diag_watch_on || !W.key_mode) return;
    uint64_t f = frame_now() + 1;
    __atomic_store_n(&W.hi, f + (uint64_t)W.frames - 1, __ATOMIC_RELAXED);
    __atomic_store_n(&W.lo, f, __ATOMIC_RELAXED);
    rt_log("watch: F12: frames %llu..%llu", (unsigned long long)f, (unsigned long long)(f + (uint64_t)W.frames - 1));
}

void diag_watch_mark_cmd(const void *cmd) {
    plat_mutex_lock(&W.lock);
    int seen = 0;
    for (int i = 0; i < W.nmarked && !seen; i++) seen = W.marked[i] == cmd;
    if (!seen && W.nmarked < (int)(sizeof W.marked / sizeof *W.marked)) W.marked[W.nmarked++] = cmd;
    plat_mutex_unlock(&W.lock);
}

// Handles of the marked recordings, so that a submit can name them.
static uint64_t ended[64];
static int nended;

void diag_watch_end_cmd(const void *cmd, uint64_t handle) {
    plat_mutex_lock(&W.lock);
    int hit = 0;
    for (int i = 0; i < W.nmarked; i++)
        if (W.marked[i] == cmd) {
            W.marked[i] = W.marked[--W.nmarked];
            hit = 1;
            break;
        }
    if (hit) {
        if (nended == (int)(sizeof ended / sizeof *ended)) memmove(ended, ended + 1, sizeof ended - sizeof *ended), nended--;
        ended[nended++] = handle;
    }
    plat_mutex_unlock(&W.lock);
    if (hit) diag_watch_note("end cmdbuf %p -> handle %#llx", cmd, (unsigned long long)handle);
}

void diag_watch_submit(const uint64_t *handles, int n) {
    if (!diag_watch_on || !in_window()) return;
    char list[256];
    size_t k = 0;
    list[0] = 0;
    plat_mutex_lock(&W.lock);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < nended; j++)
            if (ended[j] == handles[i] && k + 24 < sizeof list) {
                k += (size_t)snprintf(list + k, sizeof list - k, " [%d]=%#llx", i, (unsigned long long)handles[i]);
                break;
            }
    plat_mutex_unlock(&W.lock);
    diag_watch_note("submit #%llu: %d handles, marked:%s", (unsigned long long)nvn_submit_count() + 1, n,
                    k ? list : " none");
}
