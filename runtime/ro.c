// nn::ro, loading NRO modules at run time. On the console it talks to the ldr:ro service, which maps the image; the SDK
// then relocates it through rtld's module list and calls its init. This runtime replaces rtld, so it takes over the
// whole interface.
//
// MTF's main is a boot stub: six threads decode the game's code, one 24.7 MB NRO, out of main's data
// segment, and the stub hands it to LoadModule. The NRR in the RomFS registers its SHA-256. Two cases:
//   - the NRO was lifted (the layout has "nro"): the loader maps the image at its layout base, relocates it and seals
//     it, and LoadModule runs its DT_INIT (which runs its init_array), as RoModule::CallInit does;
//   - it was not (the first run): with --capture-nro the verified image is written to
//     build/generated/runtime/0400D4201C6EA000/1.1.1/nro-captured.nro and the run ends; recomp/nro_lift.py
//     lifts it. Without --capture-nro the run stops with that instruction.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define RO_FAIL ((uint32_t)(22 | (1 << 9)))   // a failure under the ro module (22); the game checks IsSuccess only
#define RO_NOT_NRO 0x816u                      // the SDK's result for an image without NRO0
#define MAX_REGISTERED 64
#define SETTLE_NS 50000000ull                  // kern_settle_started bound

// nn::ro::Module as the SDK fills it: +0 module object, +8 state (1 = loaded), +0x10 image, +0x18 buffer.
typedef struct { uint64_t module; uint32_t state, pad; uint64_t image, buffer; } RoModule;

static PlatMutex lock = PLAT_MUTEX_INIT;
static uint8_t registered[MAX_REGISTERED][32];
static int nregistered;
static const RtLoaded *loaded;

static void hex(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

// nn::Result nn::ro::Initialize(): the host has no ldr:ro session to open. Waits for the threads the caller started to
// reach guest code first, as the service connection takes long enough on the console (kernel.c kern_settle_started).
static void ov_initialize(Cpu *c) {
    kern_settle_started(SETTLE_NS);
    rt_log("ro: Initialize (no ldr:ro session; modules load in the host)");
    ret_x(c, 0);
}

static void ov_finalize(Cpu *c) { (void)c; }

// nn::Result nn::ro::RegisterModuleInfo(RegistrationInfo *, const void *nrr): records the NRR's hashes, which
// LoadModule checks. Version 1 (the Switch 2's) has a header 0x180 bytes longer than version 0, the same fields after
// it: the hash table offset and count at +0x4c0 (version 0: +0x340).
static void ov_register(Cpu *c) {
    const uint8_t *r = arg_p(c, 1);
    if (!r || memcmp(r, "NRR0", 4)) {
        ret_x(c, RO_FAIL);
        return;
    }
    size_t at = r[5] == 1 ? 0x4c0 : 0x340;
    uint32_t off, count;
    memcpy(&off, r + at, 4);
    memcpy(&count, r + at + 4, 4);
    plat_mutex_lock(&lock);
    for (uint32_t i = 0; i < count && nregistered < MAX_REGISTERED; i++) memcpy(registered[nregistered++], r + off + 32 * i, 32);
    plat_mutex_unlock(&lock);
    rt_log("ro: RegisterModuleInfo: NRR version %u with %u hashes", r[5], count);
    ret_x(c, 0);
}

static void ov_unregister(Cpu *c) { (void)c; }

// nn::Result nn::ro::GetBufferSize(size_t *out, const void *image): the bss size from the NRO header, as the SDK does.
static void ov_buffer_size(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    const uint8_t *h = arg_p(c, 1);
    if (!h || memcmp(h + 0x10, "NRO0", 4)) {
        ret_x(c, RO_NOT_NRO);
        return;
    }
    uint32_t bss;
    memcpy(&bss, h + 0x38, 4);
    if (out) *out = bss;
    ret_x(c, 0);
}

static int is_registered(const uint8_t sha[32]) {
    plat_mutex_lock(&lock);
    int found = 0;
    for (int i = 0; i < nregistered && !found; i++) found = !memcmp(registered[i], sha, 32);
    plat_mutex_unlock(&lock);
    return found;
}

// Where the capture goes: build/generated/runtime/... next to build/bin, or MTF_NRO_CAPTURE.
static void capture_path(char *out, size_t n) {
    const char *e = cfg_get("MTF_NRO_CAPTURE");
    if (e && *e) {
        snprintf(out, n, "%s", e);
        return;
    }
    char exe[PLAT_PATH_MAX * 3];
    int len = plat_exe_path(exe, sizeof exe);
    char *cut = len ? plat_last_sep(exe) : NULL;
    if (!cut) rt_fatal("ro: cannot find the executable's directory for the NRO capture");
    *cut = 0;
    snprintf(out, n, "%s/../generated/runtime/0400D4201C6EA000/1.1.1/nro-captured.nro", exe);
}

static void capture(const uint8_t *image, uint32_t size, const char *sha) {
    char path[PLAT_PATH_MAX * 3 + 64], dir[sizeof path];
    capture_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char *cut = plat_last_sep(dir);
    if (cut) {
        *cut = 0;
        plat_file_mkdir_p(dir);
    }
    if (!plat_file_write_atomic(path, image, size)) rt_fatal("ro: cannot write the NRO capture %s", path);
    rt_log("ro: captured the decoded NRO (%u bytes, SHA-256 %s) to %s", size, sha, path);
    rt_log("ro: lift it with tools/recomp/nro/recompile.py, then rebuild (README.md)");
    fflush(NULL);
    plat_exit(0);
}

// nn::Result nn::ro::LoadModule(Module *, const void *image, void *buffer, size_t buffer_size, int bind_flag)
static void ov_load_module(Cpu *c) {
    RoModule *rm = arg_p(c, 0);
    const uint8_t *image = arg_p(c, 1);
    if (!image || memcmp(image + 0x10, "NRO0", 4)) {
        ret_x(c, RO_NOT_NRO);
        return;
    }
    uint32_t size;
    memcpy(&size, image + 0x18, 4);
    uint8_t d[32];
    char sha[65];
    rt_sha256(image, size, d);
    hex(d, sha);
    if (!is_registered(d)) {
        rt_log("ro: LoadModule: image SHA-256 %s (%u bytes) is not registered by an NRR", sha, size);
        ret_x(c, RO_FAIL);
        return;
    }
    if (!ldr_in_layout("nro")) {
        if (rt_config()->capture_nro) capture(image, size, sha);
        rt_fatal("ro: LoadModule: the game's NRO (SHA-256 %s) is not lifted yet; run mtf-recomp.exe --capture-nro, "
                 "then tools/recomp/nro/recompile.py (README.md)", sha);
    }
    plat_mutex_lock(&lock);
    if (loaded) {
        plat_mutex_unlock(&lock);
        rt_fatal("ro: LoadModule: the NRO is loaded already");
    }
    loaded = ldr_load_image("nro", image, size);
    plat_mutex_unlock(&lock);
    if (rm) {
        rm->module = loaded->base;
        rm->state = 1;
        rm->image = (uint64_t)(uintptr_t)image;
        rm->buffer = arg_x(c, 2);
    }
    rt_log("ro: LoadModule: nro (SHA-256 %s, %u bytes, bind %s), running its DT_INIT", sha, size,
           arg_x(c, 4) & 1 ? "now" : "lazy");
    if (loaded->init) rt_call(c, loaded->init, 0);
    rt_boot_event("nro", c, "loaded");
    ret_x(c, 0);
}

// nn::Result nn::ro::LookupSymbol(uintptr_t *out, const char *name): searches every loaded module.
static void ov_lookup(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    const char *name = arg_p(c, 1);
    uint64_t a = name ? ldr_symbol(name) : 0;
    if (!a) {
        ret_x(c, RO_FAIL);
        return;
    }
    if (out) *out = a;
    ret_x(c, 0);
}

// nn::Result nn::ro::LookupModuleSymbol(uintptr_t *out, const Module *, const char *name)
static void ov_lookup_module(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    const char *name = arg_p(c, 2);
    uint64_t a = name ? ldr_symbol(name) : 0;
    if (!a || !loaded || a - loaded->base >= loaded->size) {
        ret_x(c, RO_FAIL);
        return;
    }
    if (out) *out = a;
    ret_x(c, 0);
}

static void ov_unload(Cpu *c) {
    char lr[128];
    rt_fatal("ro: UnloadModule from %s: unloading the game's NRO is not supported", ldr_describe(c->x[30], lr, sizeof lr));
}

HOST_OVERRIDE("_ZN2nn2ro10InitializeEv", ov_initialize);
HOST_OVERRIDE("_ZN2nn2ro8FinalizeEv", ov_finalize);
HOST_OVERRIDE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ov_register);
HOST_OVERRIDE("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ov_unregister);
HOST_OVERRIDE("_ZN2nn2ro13GetBufferSizeEPmPKv", ov_buffer_size);
HOST_OVERRIDE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ov_load_module);
HOST_OVERRIDE("_ZN2nn2ro12LookupSymbolEPmPKc", ov_lookup);
HOST_OVERRIDE("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", ov_lookup_module);
HOST_OVERRIDE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ov_unload);
