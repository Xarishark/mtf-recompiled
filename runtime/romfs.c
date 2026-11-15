// Clean-room directory-backed RomFS for MTF Recomp (MIT).
// Serves read-only files directly from the user's extracted RomFS directory (rt_config()->romfs)
// without duplicating the 8.17 GiB `42` archive.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "romfs_path.h"

#define NH 65536
#define READ_CHUNK (64u << 20)

typedef struct {
    PlatFile *file;
    uint64_t size;
    char rel[256];
} RomfsHandle;

static char romfs_root[2048];
static char mount_name[32];
static PlatMutex lock = PLAT_MUTEX_INIT;
static RomfsHandle handles[NH];
static int initialized;

void fs_init(void) {
    if (initialized) return;
    const char *dir = rt_config()->romfs;
    if (!dir || !*dir || !plat_dir_exists(dir))
        rt_fatal("RomFS directory missing or invalid: %s", dir ? dir : "(null)");
    if (snprintf(romfs_root, sizeof romfs_root, "%s", dir) >= (int)sizeof romfs_root)
        rt_fatal("RomFS directory path too long");
    size_t len = strlen(romfs_root);
    while (len > 1 && (romfs_root[len - 1] == '/' || romfs_root[len - 1] == '\\'))
        romfs_root[--len] = '\0';
    initialized = 1;
    rt_log("RomFS directory: %s", romfs_root);
}

void fs_trace(const char *what, const char *path) {
    if (rt_trace & RT_TRACE_FS) rt_log("fs %s: %s", what, path ? path : "(null)");
}

uint64_t romfs_file_count(void) { return 0; }
const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) {
    (void)i;
    if (offset) *offset = 0;
    if (size) *size = 0;
    return NULL;
}
const char *romfs_image_path(void) { return romfs_root; }
size_t romfs_overlay_count(void) { return 0; }
int romfs_xbox_prompts_ready(void) { return 0; }
int romfs_xbox_prompts_active(void) { return 0; }
int romfs_ps_prompts_active(void) { return 0; }
const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    (void)k;
    if (file) *file = NULL;
    if (size) *size = 0;
    if (is_added) *is_added = 0;
    return NULL;
}

int romfs_owns(const char *path) {
    if (!path || !*mount_name) return 0;
    size_t n = strlen(mount_name);
    return !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) {
    return h > ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + NH;
}

uint32_t romfs_mount(const char *name) {
    fs_init();
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_name ||
        strchr(name, '/') || !strcmp(name, "save"))
        return FS_R_INVALID_PATH;
    plat_mutex_lock(&lock);
    if (*mount_name && strcmp(mount_name, name)) {
        plat_mutex_unlock(&lock);
        rt_fatal("RomFS: mounted twice (%s then %s)", mount_name, name);
    }
    snprintf(mount_name, sizeof mount_name, "%s", name);
    plat_mutex_unlock(&lock);
    rt_log("RomFS mounted as %s:/", mount_name);
    return 0;
}

static int build_host_path(const char *rel, char *out, size_t cap) {
    return snprintf(out, cap, "%s/%s", romfs_root, rel) < (int)cap;
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("rom", path);
    if (!out) return FS_R_INVALID;
    if (mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *rel = romfs_path_relative(mount_name, path);
    if (!rel) return FS_R_PATH_NOT_FOUND;

    char full[4096];
    if (!build_host_path(rel, full, sizeof full)) return FS_R_INVALID_PATH;

    PlatFile *f = plat_file_open(full);
    if (!f) return FS_R_PATH_NOT_FOUND;

    PlatFileId id;
    if (!plat_file_id(f, &id)) {
        plat_file_close(f);
        return FS_R_INVALID;
    }

    plat_mutex_lock(&lock);
    for (size_t j = 1; j < NH; j++) {
        if (!handles[j].file) {
            handles[j].file = f;
            handles[j].size = id.size;
            snprintf(handles[j].rel, sizeof handles[j].rel, "%s", rel);
            plat_mutex_unlock(&lock);
            *out = ROMFS_HANDLE_BASE + j;
            return 0;
        }
    }
    plat_mutex_unlock(&lock);
    plat_file_close(f);
    rt_fatal("RomFS: out of file handle slots");
    return FS_R_INVALID;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (!out || offset < 0 || !romfs_handle(h)) return FS_R_INVALID;
    size_t j = (size_t)(h - ROMFS_HANDLE_BASE);

    plat_mutex_lock_shared(&lock);
    PlatFile *f = handles[j].file;
    uint64_t fsize = handles[j].size;
    plat_mutex_unlock_shared(&lock);

    if (!f || (uint64_t)offset > fsize || (!buffer && size)) return FS_R_INVALID;

    uint64_t n = size < fsize - (uint64_t)offset ? size : fsize - (uint64_t)offset;
    uint64_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(n - done);
        mem_touch((uintptr_t)buffer + done, chunk);
        mem_thaw((uintptr_t)buffer + done, chunk);
        int64_t r = plat_file_pread(f, (uint8_t *)buffer + done, chunk, (uint64_t)offset + done);
        if (r <= 0) rt_fatal("RomFS read failure at %#llx", (unsigned long long)((uint64_t)offset + done));
        done += (uint64_t)r;
    }
    *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    if (!out || !romfs_handle(h)) return FS_R_INVALID;
    size_t j = (size_t)(h - ROMFS_HANDLE_BASE);
    plat_mutex_lock_shared(&lock);
    PlatFile *f = handles[j].file;
    uint64_t fsize = handles[j].size;
    plat_mutex_unlock_shared(&lock);
    if (!f) return FS_R_INVALID;
    *out = (int64_t)fsize;
    return 0;
}

void romfs_close(uint64_t h) {
    if (!romfs_handle(h))
        rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    size_t j = (size_t)(h - ROMFS_HANDLE_BASE);
    plat_mutex_lock(&lock);
    PlatFile *f = handles[j].file;
    handles[j].file = NULL;
    handles[j].size = 0;
    handles[j].rel[0] = '\0';
    plat_mutex_unlock(&lock);
    if (!f) rt_fatal("RomFS: close of closed file handle %#llx", (unsigned long long)h);
    plat_file_close(f);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    const char *rel = romfs_path_relative(mount_name, path);
    if (!rel) return FS_R_PATH_NOT_FOUND;
    char full[4096];
    if (!build_host_path(rel, full, sizeof full)) return FS_R_INVALID_PATH;
    int kind = plat_fs_kind(full);
    if (kind == PLAT_FS_FILE) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    if (kind == PLAT_FS_DIR) {
        *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
