// Amiibo dump loader and Horizon OS NFP state emulation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiibo.h"
#include "host.h"
#include "platform/platform.h"

static PlatMutex g_amiibo_lock = PLAT_MUTEX_INIT;

// 540-byte raw NTAG215 binary dump
static uint8_t g_dump[AMIIBO_DUMP_SIZE];
static bool g_has_dump = false;
static AmiiboInfo g_info;

static bool g_device_present = true;
static bool g_detecting = false;
static bool g_tag_present = false;
static bool g_mounted = false;

// The 7-byte UID of an NTAG215 dump: bytes 0..2, then 4..7. Byte 3 is the check byte BCC0.
static void copy_uid(uint8_t out[7], const uint8_t *dump) {
    memcpy(out, dump, 3);
    memcpy(out + 3, dump + 4, 4);
}

bool amiibo_is_device_present(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_device_present;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

// While the game waits for a tag it stops and starts detection again several times a second (tens with the menu open),
// so the log names the first few and then a count every 200th start.
static uint64_t g_detect_starts;

void amiibo_start_detection(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_detecting = true;
    uint64_t n = ++g_detect_starts;
    if (n <= 3) rt_log("amiibo: start detection");
    else if (n % 200 == 0) rt_log("amiibo: detection started %llu times so far", (unsigned long long)n);
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_stop_detection(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_detecting = false;
    if (g_detect_starts <= 3) rt_log("amiibo: stop detection");
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_mount(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_device_present || !g_tag_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return false;
    }
    g_mounted = true;
    rt_log("amiibo: mounted tag '%s'", g_info.filename[0] ? g_info.filename : "unknown");
    plat_mutex_unlock(&g_amiibo_lock);
    return true;
}

void amiibo_unmount(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_mounted = false;
    rt_log("amiibo: unmounted tag");
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_is_mounted(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_mounted;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

AmiiboDeviceState amiibo_get_device_state(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_device_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_UNAVAILABLE;
    }
    if (g_mounted) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_TAG_MOUNTED;
    }
    if (g_tag_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_TAG_FOUND;
    }
    if (g_detecting) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_SEARCHING;
    }
    plat_mutex_unlock(&g_amiibo_lock);
    return AMIIBO_DEV_STATE_INITIALIZED;
}

bool amiibo_has_tag(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_has_dump && g_tag_present;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

void amiibo_get_tag_info(void *out_tag_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_tag_info;
    memset(out, 0, 0x58);
    // UUID: offset 0x00, 10 bytes max
    copy_uid(out, g_dump);
    out[0x0A] = 7;          // UUID length
    // Protocol at offset 0x20: TypeA = 1
    *(uint32_t *)(out + 0x20) = 1;
    // TagType at offset 0x24: Type2 / NTAG215 = 2
    *(uint32_t *)(out + 0x24) = 2;
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_get_model_info(void *out_model_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_model_info;
    memset(out, 0, 0x40);
    // nn::nfp::ModelInfo from the amiibo ID at 0x54: character ID and variant in the tag's byte order, series, model
    // number little-endian (big-endian on the tag), figure type. The game tells figures of one character apart by the
    // model number.
    memcpy(out + 0x00, g_dump + 0x54, 3);   // character ID, variant
    out[0x03] = g_dump[0x5A];               // series
    out[0x04] = g_dump[0x59];               // model number
    out[0x05] = g_dump[0x58];
    out[0x06] = g_dump[0x57];               // figure type
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_get_common_info(void *out_common_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_common_info;
    memset(out, 0, 0x40);
    // Last write date: 0x00..0x03
    out[0x00] = 0xE7; out[0x01] = 0x07; // 2023, u16 little-endian
    out[0x02] = 0x05; out[0x03] = 0x0C; // May 12
    // Write counter: 0x04..0x05
    out[0x04] = 0x00; out[0x05] = 0x01;
    // Version: 0x06
    out[0x06] = 0x02;
    // Application area size: 0x08..0x0B = 0xD8 (216 bytes)
    *(uint32_t *)(out + 0x08) = 0xD8;
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_get_register_info(void *out_register_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_register_info;
    memset(out, 0, 0x100);
    // Mii CharInfo at 0x00..0x57 (dummy valid mii)
    out[0x00] = 0x03; // version 3
    // First write date at 0x58..0x5B
    out[0x58] = 0xE7; out[0x59] = 0x07;   // 2023, u16 little-endian
    out[0x5A] = 0x05; out[0x5B] = 0x0C;
    // Amiibo Name at 0x5C: copy up to 40 bytes NUL-terminated
    const char *name = g_info.filename[0] ? g_info.filename : "Amiibo";
    snprintf((char *)(out + 0x5C), 0x29, "%s", name);
    plat_mutex_unlock(&g_amiibo_lock);
}

int amiibo_get_application_area(void *out_buf, uint32_t max_size) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_has_dump || !out_buf) {
        plat_mutex_unlock(&g_amiibo_lock);
        return 0;
    }
    uint32_t to_copy = max_size < 0xD8 ? max_size : 0xD8;
    // Decrypted/payload application area starts at offset 0xDC (or 0x0A0..0x178 in raw dump)
    if (0xDC + to_copy <= AMIIBO_DUMP_SIZE) {
        memcpy(out_buf, g_dump + 0xDC, to_copy);
    } else {
        memset(out_buf, 0, to_copy);
    }
    plat_mutex_unlock(&g_amiibo_lock);
    return (int)to_copy;
}
