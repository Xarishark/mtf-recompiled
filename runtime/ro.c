// nn::ro (dynamic NRO module loading) for Switch 2 / Mario Tennis Fever.
// Clean-room MIT implementation following the Switch 2 NRR v1 and nn::ro::LoadModule
// behavior documented in seedds/nxnative (dm/host/hle_ro.c, MIT).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "sha256.h"

#define RO_FAIL    ((uint32_t)(22u | (1u << 9)))
#define RO_NOT_NRO ((uint32_t)0x816u)

static PlatMutex g_ro_lock = PLAT_MUTEX_INIT;

static void sha256_hex(const uint8_t sha[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", sha[i]);
    out[64] = '\0';
}

static void h_ro_Initialize(Cpu *c) {
    rt_log("ro::Initialize (HLE)");
    c->x[0] = 0;
}

static void h_ro_Finalize(Cpu *c) {
    (void)c;
}

static void h_ro_RegisterModuleInfo(Cpu *c) {
    uint64_t nrr_addr = c->x[1];
    if (!nrr_addr || !plat_mem_readable(nrr_addr, 0x4D0)) {
        c->x[0] = RO_FAIL;
        return;
    }
    const uint8_t *r = (const uint8_t *)(uintptr_t)nrr_addr;
    if (memcmp(r, "NRR0", 4) != 0) {
        c->x[0] = RO_FAIL;
        return;
    }
    uint8_t ver = r[5];
    size_t hdr = (ver == 1) ? 0x4C0 : 0x340;
    uint32_t off = *(const uint32_t *)(r + hdr);
    uint32_t cnt = *(const uint32_t *)(r + hdr + 4);
    rt_log("ro::RegisterModuleInfo: NRR v%u, hash_off=%#x, count=%u", ver, off, cnt);
    c->x[0] = 0;
}

static void h_ro_GetBufferSize(Cpu *c) {
    uint64_t out_addr = c->x[0];
    uint64_t img_addr = c->x[1];
    if (!img_addr || !plat_mem_readable(img_addr, 0x40)) {
        c->x[0] = RO_NOT_NRO;
        return;
    }
    const uint8_t *h = (const uint8_t *)(uintptr_t)img_addr;
    if (memcmp(h + 0x10, "NRO0", 4) != 0) {
        c->x[0] = RO_NOT_NRO;
        return;
    }
    uint32_t bss_size = *(const uint32_t *)(h + 0x38);
    if (out_addr && plat_mem_readable(out_addr, 8)) {
        ST64(out_addr, (uint64_t)bss_size);
    }
    c->x[0] = 0;
}

static void capture_nro_and_exit(const uint8_t *h, uint32_t n, const uint8_t sha[32]) {
    const char *dir = getenv("MTF_NRO_CAPTURE");
    if (!dir || !*dir) return;
    char hex[65], path[1024], tmp[1040];
    sha256_hex(sha, hex);
    snprintf(path, sizeof path, "%s/%s.nro", dir, hex);
    snprintf(tmp, sizeof tmp, "%s.partial", path);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(h, 1, n, f) != n || fclose(f) != 0) {
        rt_fatal("ro::LoadModule: failed to write captured NRO to %s", tmp);
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        rt_fatal("ro::LoadModule: failed to rename %s -> %s", tmp, path);
    }
    rt_log("ro::LoadModule: captured NRO0 (%u bytes, sha256=%s) -> %s", n, hex, path);
    fflush(stdout);
    fflush(stderr);
    plat_exit(0);
}

static void h_ro_LoadModule(Cpu *c) {
    uint64_t mod_addr = c->x[0];
    uint64_t img_addr = c->x[1];
    uint64_t buf_addr = c->x[2];
    uint64_t buf_size = c->x[3];
    uint32_t flag = (uint32_t)c->x[4];

    if (!img_addr || !plat_mem_readable(img_addr, 0x40)) {
        c->x[0] = RO_NOT_NRO;
        return;
    }
    const uint8_t *h = (const uint8_t *)(uintptr_t)img_addr;
    if (memcmp(h + 0x10, "NRO0", 4) != 0) {
        c->x[0] = RO_NOT_NRO;
        return;
    }
    uint32_t n = *(const uint32_t *)(h + 0x18);
    if (!plat_mem_readable(img_addr, n)) {
        c->x[0] = RO_NOT_NRO;
        return;
    }

    uint8_t sha[32];
    sha256(h, n, sha);
    char hex[65];
    sha256_hex(sha, hex);

    plat_mutex_lock(&g_ro_lock);
    rt_log("ro::LoadModule: NRO0 size=%u sha256=%s buf=%#llx size=%#llx flag=%d mod=%#llx",
           n, hex, (unsigned long long)buf_addr, (unsigned long long)buf_size, (int)flag,
           (unsigned long long)mod_addr);
    capture_nro_and_exit(h, n, sha);
    plat_mutex_unlock(&g_ro_lock);

    const RtLoaded *nro = NULL;
    for (int i = 0; i < ldr_count(); i++) {
        if (!strcmp(ldr_module(i)->name, "nro0")) { nro = ldr_module(i); break; }
    }
    if (nro) {
        extern void rt_init_nro_module(Cpu *c, const char *name);
        rt_init_nro_module(c, "nro0");
        if (mod_addr && plat_mem_readable(mod_addr, 8)) ST64(mod_addr, nro->base);
        c->x[0] = 0;
        return;
    }

    rt_fatal("ro::LoadModule: no pre-lifted NRO registered for sha256=%s (run with MTF_NRO_CAPTURE first)", hex);
}

static void h_ro_LookupSymbol(Cpu *c) {
    uint64_t out_addr = c->x[0];
    uint64_t name_addr = c->x[1];
    const char *name = name_addr ? (const char *)(uintptr_t)name_addr : NULL;
    uint64_t sym = name ? ldr_symbol(name) : 0;
    if (!sym) {
        c->x[0] = RO_FAIL;
        return;
    }
    if (out_addr && plat_mem_readable(out_addr, 8)) ST64(out_addr, sym);
    c->x[0] = 0;
}

static void h_ro_LookupModuleSymbol(Cpu *c) {
    uint64_t out_addr = c->x[0];
    uint64_t name_addr = c->x[2];
    const char *name = name_addr ? (const char *)(uintptr_t)name_addr : NULL;
    uint64_t sym = name ? ldr_symbol(name) : 0;
    if (!sym) {
        c->x[0] = RO_FAIL;
        return;
    }
    if (out_addr && plat_mem_readable(out_addr, 8)) ST64(out_addr, sym);
    c->x[0] = 0;
}

static void h_ro_UnloadModule(Cpu *c) {
    (void)c;
}

HOST_OVERRIDE("_ZN2nn2ro10InitializeEv", h_ro_Initialize);
HOST_OVERRIDE("_ZN2nn2ro8FinalizeEv", h_ro_Finalize);
HOST_OVERRIDE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", h_ro_RegisterModuleInfo);
HOST_OVERRIDE("_ZN2nn2ro13GetBufferSizeEPmPKv", h_ro_GetBufferSize);
HOST_OVERRIDE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", h_ro_LoadModule);
HOST_OVERRIDE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib", h_ro_LoadModule);
HOST_OVERRIDE("_ZN2nn2ro12LookupSymbolEPmPKc", h_ro_LookupSymbol);
HOST_OVERRIDE("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", h_ro_LookupModuleSymbol);
HOST_OVERRIDE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", h_ro_UnloadModule);
