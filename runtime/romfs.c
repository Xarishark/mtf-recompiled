// The game's read-only RomFS, served from the extracted RomFS directory (tools/nup_extract.py writes the files of the
// update's RomFS as they are: MTF 1.1.1 has one 8.2 GB archive, "42", and .nrr/nrr.nrr).
//
// fs_host.c routes every path on the mounted name ("rom:/...") and every handle at or
// above ROMFS_HANDLE_BASE here. Reads are positional on a shared handle per file, so guest threads read in parallel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "romfs_path.h"

#define MAX_OPEN 4096
#define MAX_FILES 64

typedef struct { char path[256]; uint64_t size; } Entry;   // path relative to the RomFS root

static Entry files[MAX_FILES];
static uint64_t file_count;
static char root[PLAT_PATH_MAX * 3];
static char mount_name[32];
static PlatMutex lock = PLAT_MUTEX_INIT;
static struct { PlatFile *f; uint64_t size; int used; } open_files[MAX_OPEN];
static int initialized;

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

void fs_trace(const char *what, const char *path) {
    static double t0;
    if (!(rt_trace & RT_TRACE_FS)) return;
    if (!t0) t0 = now_s();
    rt_log("fs %8.3f %s %s", now_s() - t0, what, path ? path : "(null)");
}

// The file list, for the probe and the log: the archive and the NRR, listed by name.
static void add_file(const char *relative) {
    char full[sizeof root + 300];
    snprintf(full, sizeof full, "%s/%s", root, relative);
    int64_t size = plat_file_size(full);
    if (size < 0 || file_count == MAX_FILES) return;
    snprintf(files[file_count].path, sizeof files[file_count].path, "%s", relative);
    files[file_count++].size = (uint64_t)size;
}

static int list_top(const char *name, void *ctx) {
    (void)ctx;
    add_file(name);
    return 1;
}

void fs_init(void) {
    plat_mutex_lock(&lock);
    if (initialized) {
        plat_mutex_unlock(&lock);
        return;
    }
    snprintf(root, sizeof root, "%s", rt_config()->romfs);
    if (!plat_dir_exists(root)) rt_fatal("romfs: %s is not a directory (extract the game first: README.md)", root);
    plat_file_list(root, "", list_top, NULL);
    add_file(".nrr/nrr.nrr");
    if (!file_count) rt_fatal("romfs: %s holds no files", root);
    uint64_t total = 0;
    for (uint64_t i = 0; i < file_count; i++) total += files[i].size;
    rt_log("romfs: %s, %llu files, %.1f GiB", root, (unsigned long long)file_count, (double)total / (1ull << 30));
    initialized = 1;
    plat_mutex_unlock(&lock);
}

uint32_t romfs_mount(const char *name) {
    if (!name || !*name || strlen(name) >= sizeof mount_name) return FS_R_INVALID;
    fs_init();
    plat_mutex_lock(&lock);
    if (*mount_name && strcmp(mount_name, name)) {
        plat_mutex_unlock(&lock);
        rt_fatal("romfs: MountRom(\"%s\") after MountRom(\"%s\"): one RomFS mount is supported", name, mount_name);
    }
    snprintf(mount_name, sizeof mount_name, "%s", name);
    plat_mutex_unlock(&lock);
    rt_log("romfs: mounted as %s:", name);
    return 0;
}

int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) { return h >= ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + MAX_OPEN; }

static void host_path(char *out, size_t n, const char *relative) { snprintf(out, n, "%s/%s", root, relative); }

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("open", path);
    if (mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *relative = romfs_path_relative(mount_name, path);
    if (!relative) return FS_R_PATH_NOT_FOUND;
    char full[sizeof root + 512];
    host_path(full, sizeof full, relative);
    if (!plat_file_exists(full)) return FS_R_PATH_NOT_FOUND;
    PlatFile *f = plat_file_open(full);
    if (!f) return FS_R_PATH_NOT_FOUND;
    int64_t size = plat_file_size(full);
    plat_mutex_lock(&lock);
    for (uint64_t j = 0; j < MAX_OPEN; j++) {
        if (open_files[j].used) continue;
        open_files[j].used = 1;
        open_files[j].f = f;
        open_files[j].size = size < 0 ? 0 : (uint64_t)size;
        plat_mutex_unlock(&lock);
        *out = ROMFS_HANDLE_BASE + j;
        return 0;
    }
    plat_mutex_unlock(&lock);
    plat_file_close(f);
    rt_fatal("romfs: more than %d files open", MAX_OPEN);
}

static PlatFile *file_of(uint64_t h, uint64_t *size) {
    if (!romfs_handle(h)) return NULL;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock(&lock);
    PlatFile *f = open_files[j].used ? open_files[j].f : NULL;
    if (size) *size = open_files[j].size;
    plat_mutex_unlock(&lock);
    return f;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    uint64_t file_size;
    PlatFile *f = file_of(h, &file_size);
    if (!f || offset < 0 || (!buffer && size)) return FS_R_INVALID;
    if ((uint64_t)offset > file_size) return FS_R_INVALID;
    uint64_t want = file_size - (uint64_t)offset < size ? file_size - (uint64_t)offset : size;
    mem_thaw((uint64_t)(uintptr_t)buffer, want);   // a frozen guest region would refuse the write
    uint64_t done = 0;
    while (done < want) {
        uint32_t chunk = want - done > 0x40000000ull ? 0x40000000u : (uint32_t)(want - done);
        mem_touch((uintptr_t)buffer + done, chunk);
        int64_t got = plat_file_pread(f, (uint8_t *)buffer + done, chunk, (uint64_t)offset + done);
        if (got < 0)
            rt_fatal("romfs: read of %u bytes at %#llx failed (%s)", chunk, (unsigned long long)(offset + done),
                     got == -2 ? "the guest buffer is read-only" : "I/O error");
        if (!got) break;
        done += (uint64_t)got;
    }
    if (out) *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    uint64_t size;
    if (!file_of(h, &size) || !out) return FS_R_INVALID;
    *out = (int64_t)size;
    return 0;
}

void romfs_close(uint64_t h) {
    if (!romfs_handle(h)) return;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock(&lock);
    PlatFile *f = open_files[j].used ? open_files[j].f : NULL;
    open_files[j].used = 0;
    open_files[j].f = NULL;
    plat_mutex_unlock(&lock);
    if (f) plat_file_close(f);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    const char *relative = romfs_path_relative(mount_name, path);
    if (!relative && romfs_owns(path)) {   // the root itself: "rom:/" or "rom:"
        if (out) *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    if (!relative) return FS_R_PATH_NOT_FOUND;
    char full[sizeof root + 512];
    host_path(full, sizeof full, relative);
    if (plat_file_exists(full)) {
        if (out) *out = FS_ENTRY_FILE;
        return 0;
    }
    if (plat_dir_exists(full)) {
        if (out) *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

// ---- nn::fs overrides for the RomFS and the file system session
// nn::Result nn::fs::MountRom(const char *name, void *cache, size_t cache_size)
static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
// nn::Result nn::fs::QueryMountRomCacheSize(size_t *out): no cache needed, the host reads the files directly
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
// void nn::fs::InitializeForApplication(): SDK-internal; finds the RomFS instead of opening the fs service session
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}
// bool nn::fs::detail::IsEnabledAccessLog(...): the access log goes to the fs service, which the host does not have;
// asking for it opens that session (nn::fs::SetAllocator does, from the boot stub). Always off.
static void ov_access_log_off(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
HOST_OVERRIDE("_ZN2nn2fs6detail18IsEnabledAccessLogEj", ov_access_log_off);
HOST_OVERRIDE("_ZN2nn2fs6detail18IsEnabledAccessLogEv", ov_access_log_off);
