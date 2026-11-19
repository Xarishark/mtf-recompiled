// The boot driver: the guest main thread runs the game's own nn::init::Start with three host callbacks in place of
// rtld's, the second of which runs DT_INIT of main and sdk; Start then calls nnMain.
//
// rtld on the console ends with nn::init::Start(thread handle, first module base, loader_ready, module_inits,
// module_finis); the second argument is the address right after rtld's own 0x20000-byte image, which is main.
// nnosInitialize walks the module headers from there. Start runs _init_libc0, nnosInitialize, loader_ready,
// _init_libc1, nninitInitializeSdkModule, nninitStartup, _init_libc2, module_inits, nnMain, module_finis.
// Contains adapted code under the MIT licence: authors and licence texts in THIRD_PARTY_NOTICES.md.
#include "vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "diag_watch.h"
#include "host.h"
#include "platform/platform.h"

#define SYM_INIT_START "_ZN2nn4init5StartEmmPFvvES2_S2_"
#define CTOR_DONE 2   // a module's constructor flag after its DT_INIT ran the init_array

// DT_INIT order and init_array sizes of v1.1.1 (DT_INIT_ARRAYSZ / 8): main before sdk, as the constructor flags
// confirm.
typedef struct { const char *name; uint64_t entries; } InitCount;
static const InitCount expected_inits[] = {{"main", 7}, {"sdk", 91}};

static volatile int32_t inits_done;
uint64_t rt_guest_start_ns;

static const RtLoaded *module_named(const char *name) {
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, name)) return ldr_module(i);
    rt_fatal("boot: module %s is not loaded", name);
}

static uint32_t ctor_state(const RtLoaded *m) { return m->ctor_flag ? LD32(m->ctor_flag) : CTOR_DONE; }

static const char *caller(const Cpu *c, char *buf, int n) { return ldr_describe(c->x[30] - 4, buf, n); }

// main.npdm META: the main thread's priority (+0xE), core (+0xF) and stack size (+0x1C). kernel.c builds the main
// thread with these values.
static void check_npdm(void) {
    size_t n;
    uint8_t *npdm = ldr_read_verified(rt_config()->exefs, "main.npdm", &n);
    if (n < 0x80 || memcmp(npdm, "META", 4)) rt_fatal("boot: main.npdm has no META header");
    uint32_t stack;
    memcpy(&stack, npdm + 0x1C, 4);
    int prio = npdm[0xE], core = npdm[0xF];
    free(npdm);
    if (prio != 44 || core != 0 || stack != 0x100000)
        rt_fatal("boot: main.npdm asks for priority %d, core %d, stack %#x; kernel.c builds 44, 0, 0x100000", prio,
                 core, stack);
}

// ---- the three callbacks of nn::init::Start (rtld's job on hardware)
static void cb_loader_ready(Cpu *c) {
    (void)c;
    rt_log("boot: rtld callback: nnosInitialize finished, modules linked");
}

// DT_INIT registers the module with musl (__nnmusl_init_dso) and runs its init_array; the constructor flag reads 2
// afterwards.
static void cb_module_inits(Cpu *c) {
    if (plat_cas32(&inits_done, 0, 0)) rt_fatal("boot: module constructors requested twice");
    for (unsigned k = 0; k < sizeof expected_inits / sizeof *expected_inits; k++) {
        const RtLoaded *m = module_named(expected_inits[k].name);
        uint64_t entries = m->init_array_size / 8;
        if (!m->init) rt_fatal("boot: %s has no DT_INIT", m->name);
        if (entries != expected_inits[k].entries)
            rt_fatal("boot: %s has %llu init_array entries, v1.1.1 has %llu", m->name, (unsigned long long)entries,
                     (unsigned long long)expected_inits[k].entries);
        rt_log("boot: ENTER %s DT_INIT (%llu init_array entries)", m->name, (unsigned long long)entries);
        rt_call(c, m->init, 0);
        uint32_t state = ctor_state(m);
        rt_log("boot: DT_INIT %s done, constructor flag %u", m->name, state);
        if (state != CTOR_DONE) rt_fatal("boot: %s DT_INIT left its constructor flag at %u, not 2", m->name, state);
    }
    plat_xchg32(&inits_done, 1);
}

static void cb_module_finis(Cpu *c) {
    char lr[128];
    rt_fatal("boot: the game asked for module finalizers (from %s): the guest exited", caller(c, lr, sizeof lr));
}

// ---- checkpoints
static void stats_line(void) {
    RtStats s = rt_stats();
    rt_logf("boot: %llu SVCs, %llu threads, %llu host calls\n", (unsigned long long)s.svcs,
            (unsigned long long)s.threads, (unsigned long long)s.host_calls);
}

void rt_boot_event(const char *what, Cpu *c, const char *detail) {
    char at[128];
    caller(c, at, sizeof at);
    rt_log("boot: %s %s from %s", what, detail ? detail : "", at);
    if (!strcmp(what, "nnMain") && !plat_cas32(&inits_done, 0, 0)) rt_fatal("boot: nnMain before the module DT_INITs");
    if (!strcmp(what, "end")) {
        stats_line();
        fflush(stdout);
        fflush(stderr);
        plat_exit(0);   // other guest threads are still running; nothing of theirs needs an orderly end
    }
}

// ---- start
static uint64_t cb_addr[3];

static void guest_main(Cpu *c) {
    uint32_t handle = kern_main_thread(&c);
    uint64_t start = ldr_symbol(SYM_INIT_START);
    if (!start) rt_fatal("boot: %s is not exported", SYM_INIT_START);
    char d[128];
    rt_log("boot: ENTER nn::init::Start at %s, main thread %#x", ldr_describe(start, d, sizeof d), handle);
    rt_call(c, start, 5, (uint64_t)handle, module_named("main")->base, cb_addr[0], cb_addr[1], cb_addr[2]);
    rt_fatal("boot: nn::init::Start returned");
}

static void prewarm_main(void *arg) {
    (void)arg;
    vk_pipeline_prewarm();
}

void rt_boot(void) {
    check_npdm();
    // Pipelines recorded in earlier runs are built while the modules load; the guest waits for them. Not in the --boot
    // runs (the installer's NRO capture and the probes), which never draw.
    int warm_up = !rt_config()->boot;
    if (warm_up) vk_pipeline_async_enable();
    PlatThread *warm = warm_up ? plat_thread_new(prewarm_main, NULL, "prewarm") : NULL;
    ldr_load(rt_config()->exefs);
    cb_addr[0] = hostfn_register("<rtld loader_ready>", cb_loader_ready);
    cb_addr[1] = hostfn_register("<rtld module_inits>", cb_module_inits);
    cb_addr[2] = hostfn_register("<rtld module_finis>", cb_module_finis);
    hostfn_seal();
    diag_watch_init();   // MTF_WATCH: needs the lifted modules linked
    uint64_t held = plat_now_ms();
    if (warm) plat_thread_join(warm);
    else if (warm_up) vk_pipeline_prewarm();
    held = plat_now_ms() - held;
    rt_log("boot: guest starts %llu ms after process start (%llu ms of it waiting for the prewarm)",
           (unsigned long long)(plat_process_uptime_ns() / 1000000), (unsigned long long)held);
    rt_guest_start_ns = kern_now_ns();
    kern_run_on_guest_main(guest_main);
    rt_fatal("boot: the guest main thread ended");
}
