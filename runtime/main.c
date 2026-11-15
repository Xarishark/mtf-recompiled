// MTF Recomp entry point: reserves guest address space, installs crash handler,
// parses CLI arguments, and starts Horizon OS boot (runtime/boot.c).
// Adapted from totk-pc-recomp runtime/main.c (MIT).

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"
#include "sha256.h"

unsigned rt_trace;
static RtConfig g_cfg;

const RtConfig *rt_config(void) { return &g_cfg; }
const char *rt_log_path(void) { return NULL; }
void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

void rt_log_raw(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (k > 0 ? (k < (int)sizeof buf - n - 1 ? (size_t)k : sizeof buf - n - 2) : 0);
    buf[len++] = '\n';
    rt_log_raw(buf, len);
}

void rt_oom(size_t bytes, const char *what) {
    rt_fatal("out of memory (%zu bytes for %s)", bytes, what ? what : "?");
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char d[160];
    rt_log("FATAL: guest brk #%#x at %s", imm, ldr_describe(pc, d, sizeof d));
    kern_backtrace(c);
    plat_exit(1);
}

void rt_udf(Cpu *c, uint64_t pc) {
    char d[160];
    rt_log("FATAL: guest udf at %s", ldr_describe(pc, d, sizeof d));
    kern_backtrace(c);
    plat_exit(1);
}

uint64_t rt_ticks(void) {
    // Switch 2 physical counter runs at 31.25 MHz (1 tick = 32 ns).
    return (plat_now_ns() * 31250000ull) / 1000000000ull;
}

uint64_t rt_ns_to_ticks(uint64_t ns) {
    return (uint64_t)(((__uint128_t)ns * 31250000ull) / 1000000000ull);
}

uint64_t rt_ticks_to_ns(uint64_t ticks) {
    return (uint64_t)(((__uint128_t)ticks * 1000000000ull) / 31250000ull);
}

volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    (void)addr;
    (void)lr;
    (void)x0;
}

#ifdef _WIN32
#include <windows.h>
#endif

static void crash_handler(const PlatCrash *c) {
    uintptr_t base = plat_exe_base();
#ifdef _WIN32
    HMODULE mod = NULL;
    char modname[260] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)c->rip, &mod) && mod) {
        GetModuleFileNameA(mod, modname, sizeof modname);
        rt_log("CRASH: exception %#x at host rip=%#llx (%s+%#llx), addr=%#llx",
               c->code, (unsigned long long)c->rip, modname,
               (unsigned long long)(c->rip - (uintptr_t)mod), (unsigned long long)c->addr);
        fflush(stderr);
        return;
    }
#endif
    rt_log("CRASH: exception %#x at host rip=%#llx (exe+%#llx), addr=%#llx",
           c->code, (unsigned long long)c->rip, (unsigned long long)(c->rip - base),
           (unsigned long long)c->addr);
    fflush(stderr);
}

static void require_cpu(void) {
#if defined(__x86_64__)
    __builtin_cpu_init();
#if defined(__AVX2__)
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma") || !__builtin_cpu_supports("bmi2")) {
        fprintf(stderr, "CPU lacks x86-64-v3 (AVX2/FMA/BMI2)\n");
        exit(1);
    }
#endif
#endif
}

static void parse_trace(const char *s) {
    if (!s || !*s) return;
    static const struct { const char *name; unsigned bit; } bits[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"codec", RT_TRACE_CODEC},
    };
    const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        const char *q = p;
        while (*q && *q != ',' && *q != ' ') q++;
        size_t n = (size_t)(q - p);
        if (n == 3 && !strncmp(p, "all", 3)) {
            rt_trace = 0x3ffu;
        } else {
            for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
                if (strlen(bits[i].name) == n && !strncmp(p, bits[i].name, n)) rt_trace |= bits[i].bit;
            }
        }
        p = q;
    }
}

int main(int argc, char **argv) {
    require_cpu();
    mem_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    parse_trace(getenv("TOTK_TRACE"));

    g_cfg = (RtConfig){
        .exefs = rt_default_exefs,
        .romfs = rt_default_romfs,
        .profile = rt_default_profile,
        .gpu = cfg_get("MTF_GPU"),
    };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exefs") && i + 1 < argc) g_cfg.exefs = argv[++i];
        else if (!strcmp(argv[i], "--romfs") && i + 1 < argc) g_cfg.romfs = argv[++i];
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) g_cfg.profile = argv[++i];
        else if (!strcmp(argv[i], "--boot")) g_cfg.boot = 1;
        else if (!strcmp(argv[i], "--version")) {
            printf("mtf-recomp %s (%s %s)\n", rt_port_version, "Mario Tennis Fever", rt_game_display);
            return 0;
        }
    }

    rt_log("mtf-recomp %s: exefs=%s romfs=%s profile=%s",
           rt_port_version, g_cfg.exefs, g_cfg.romfs, g_cfg.profile);
    rt_boot();
    return 0;
}
