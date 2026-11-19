// NVN proc table. Every name the game's proc loader asks for gets a synthetic host
// address through hostfn_register before hostfn_seal; nvnBootstrapLoader and nvnDeviceGetProcAddress hand them out.
// nvnp_<name> is the implementation of one proc: the weak definitions here stop with the proc's name, nvn_api_*.c
// and nvn_ext.c define the strong ones. The name list is runtime/nvn_procs.inc (tools/gen_nvn_procs.py).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

// The strong nvnp_nvnDeviceGetProcAddress below replaces the stand-in generated for the same row.
#define nvnp_nvnDeviceGetProcAddress nvnp_stub_nvnDeviceGetProcAddress

#define NVN_PROC(n) void nvnp_##n(Cpu *c);
#include "nvn_procs.inc"
#undef NVN_PROC

#define NVN_PROC(n)                                                                                                  \
    __attribute__((weak)) void nvnp_##n(Cpu *c) {                                                                     \
        (void)c;                                                                                                     \
        rt_fatal("NVN " #n " is not implemented");                                                                   \
    }
#include "nvn_procs.inc"
#undef NVN_PROC
#undef nvnp_nvnDeviceGetProcAddress
void nvnp_nvnDeviceGetProcAddress(Cpu *c);

typedef struct { const char *name; uint64_t addr; } Proc;

#define NVN_PROC(n) +1
enum { PROC_COUNT = 0
#include "nvn_procs.inc"
};
#undef NVN_PROC

static Proc procs[PROC_COUNT];
static int nprocs;

static int by_name(const void *a, const void *b) { return strcmp(((const Proc *)a)->name, ((const Proc *)b)->name); }

void nvn_procs_register(void) {
    if (nprocs) return;
#define NVN_PROC(n) procs[nprocs++] = (Proc){#n, hostfn_register("nvn:" #n, nvnp_##n)};
#include "nvn_procs.inc"
#undef NVN_PROC
    qsort(procs, (size_t)nprocs, sizeof *procs, by_name);
}

uint64_t nvn_proc_address(const char *name) {
    if (!name) return 0;
    const Proc *p = bsearch(&(Proc){name, 0}, procs, (size_t)nprocs, sizeof *procs, by_name);
    if (p) return p->addr;
    // Unknown names get NULL, as on hardware; each distinct one is logged once.
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static char seen[32][96];
    static int nseen;
    plat_mutex_lock(&lock);
    int fresh = 1;
    for (int i = 0; i < nseen; i++)
        if (!strncmp(seen[i], name, sizeof seen[i] - 1)) fresh = 0;
    if (fresh && nseen < 32) {
        strncpy(seen[nseen], name, sizeof seen[0] - 1);
        nseen++;
    } else {
        fresh = 0;
    }
    plat_mutex_unlock(&lock);
    if (fresh) rt_log("nvn: no such proc \"%s\", returning NULL", name);
    return 0;
}

// PFNNVNGENERICFUNC nvnDeviceGetProcAddress(const NVNdevice *device, const char *name)
void nvnp_nvnDeviceGetProcAddress(Cpu *c) {
    const char *name = arg_p(c, 1);
    ret_x(c, nvn_proc_address(name));
}

// MTF_PROC_SCAN=<file>: the variables the game calls each proc through. Once the game has loaded its procs (the 120th
// present), every 8-byte word of the loaded modules that holds a proc's address is written as "proc <name> <address>",
// and every word that holds one of those addresses as "ref <variable> <address>" (the global offset table entries the
// code loads the variables through). tools/nvn_proc_usage.py pairs them with the lifted code that reads them.
static int by_addr(const void *a, const void *b) {
    uint64_t x = ((const Proc *)a)->addr, y = ((const Proc *)b)->addr;
    return x < y ? -1 : x > y;
}
static int by_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// Calls fn for each readable, 8-byte aligned word of the loaded modules.
static void each_module_word(void (*fn)(uint64_t at, uint64_t v, void *ctx), void *ctx) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        for (uint64_t page = m->base; page < m->base + m->size; page += 4096) {
            if (!mem_guest_readable(page, 4096)) continue;
            for (uint64_t a = page; a < page + 4096; a += 8) fn(a, *(const uint64_t *)(uintptr_t)a, ctx);
        }
    }
}

typedef struct { FILE *f; const Proc *sorted; uint64_t *vars; size_t nvars, cap; } ScanCtx;

static void find_procs(uint64_t at, uint64_t v, void *p) {
    ScanCtx *s = p;
    const Proc *hit = bsearch(&(Proc){NULL, v}, s->sorted, (size_t)nprocs, sizeof *s->sorted, by_addr);
    if (!v || !hit) return;
    fprintf(s->f, "proc %s %#llx\n", hit->name, (unsigned long long)at);
    if (s->nvars == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1024;
        uint64_t *n = realloc(s->vars, cap * sizeof *n);
        if (!n) return;
        s->vars = n, s->cap = cap;
    }
    s->vars[s->nvars++] = at;
}

static void find_refs(uint64_t at, uint64_t v, void *p) {
    ScanCtx *s = p;
    if (v && bsearch(&v, s->vars, s->nvars, sizeof *s->vars, by_u64))
        fprintf(s->f, "ref %#llx %#llx\n", (unsigned long long)v, (unsigned long long)at);
}

void nvn_procs_scan(void) {
    const char *path = cfg_get("MTF_PROC_SCAN");
    if (!path || !*path || !nprocs) return;
    FILE *f = fopen(path, "w");
    Proc *sorted = malloc(sizeof *sorted * (size_t)nprocs);
    if (!f || !sorted) {
        rt_log("[RED] nvn: MTF_PROC_SCAN: cannot write %s", path);
        if (f) fclose(f);
        free(sorted);
        return;
    }
    memcpy(sorted, procs, sizeof *sorted * (size_t)nprocs);
    qsort(sorted, (size_t)nprocs, sizeof *sorted, by_addr);
    ScanCtx s = {f, sorted, NULL, 0, 0};
    each_module_word(find_procs, &s);
    qsort(s.vars, s.nvars, sizeof *s.vars, by_u64);
    each_module_word(find_refs, &s);
    fclose(f);
    rt_log("nvn: MTF_PROC_SCAN: %zu words hold a proc's address; written to %s", s.nvars, path);
    free(s.vars);
    free(sorted);
}
