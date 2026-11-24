// Shaders: the game's NVN2 shaders, translated ahead of time to SPIR-V (tools/shaders/translate.py), and the runtime's
// own (gfx_shader_own). Each comes with its reflection: the <key>.res.json the translator writes (buffers, textures and
// images with their bindings and constant-buffer slots) and what the SPIR-V itself declares (interface Locations, the
// workgroup size). Loaded shaders stay in a memory cache for the life of the process.
//
// A shader is named by its key: the first 16 hex digits of SHA-256 over the control section's first 0x48 bytes
// (+0x30..+0x34 zeroed) and the code area, the same key tools/shaders/extract.py gives the shader it finds in the game's
// files. The key is computed once per program and kept in the program's copy of the control section (nvn2_ctl.h).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "nvn2_ctl.h"
#include "sha256.h"

#define BUCKETS 4096
#define PATH_CAP 1024                                      // bytes of a UTF-8 path

typedef struct {                                           // which answers a shader's translation depends on
    uint32_t slots;                                        // tex_type, img_type, img_format of these c2 slots
    uint32_t attrs;                                        // attr_type (VS input Locations)
    uint32_t fouts;                                        // frag_out_type (FS output Locations)
} UseMask;

struct GfxShader {
    struct GfxShader *next;
    uint8_t key[32];
    uint8_t iid[32];                                       // input id: the key for a game shader, zero for the runtime's
    uint32_t *spirv;
    uint64_t words;
    char *reflection;                                      // the .res.json text, parsed into the fields below
    uint64_t reflection_size;
    // Reflection (reflect(), once per shader before it enters the cache)
    int stage;                                             // GFX_STAGE_*
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    uint32_t in_mask, out_mask;                            // user Locations of Input / Output variables
    GfxShaderIo in[32], out[32];
    uint32_t local[3];
    uint32_t mvprev_banks;                                 // .res.json "mvprev_banks", 0 none
};

static PlatMutex cache_lock = PLAT_MUTEX_INIT;             // memory cache
static GfxShader *buckets[BUCKETS];
static volatile int64_t n_hits, n_failures, n_keys;       // n_keys: memory cache entries

#define read_file plat_file_read

// ---- memory cache

static GfxShader *mem_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    free(s->spirv), free(s->reflection), free(s);
}

static void reflect(GfxShader *s);

// Inserts s, or frees it and returns the entry another thread inserted first.
static GfxShader *mem_insert(GfxShader *s) {
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
    }
    plat_mutex_unlock(&cache_lock);
    return s;
}

// A .res.json the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
// left zero-filled (3647 of 21924 in one cache were) fails this; reflect() would take it for a vertex shader with
// no resources, and its pipeline layout would lack the shader's buffers and textures.
static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4) ||
                  !strncmp(st, "\"gs\"", 4) || !strncmp(st, "\"hs\"", 4) ||
                  !strncmp(st, "\"ds\"", 4));   // geometry, hull and domain shaders (vk_pipeline.c)
}

// Every instruction after the header has a word count and the last one ends at the module's end: a module with
// zero-filled clusters fails.
static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

const char *gfx_shader_reflection(const GfxShader *s) { return s ? s->reflection : NULL; }

const GfxShader *gfx_shader_own(const uint32_t *spirv, uint64_t words, const char *res_json) {
    if (!res_json || words < 5 || spirv[0] != 0x07230203 || !spirv_complete(spirv, words) ||
        !reflection_valid(res_json, strlen(res_json)))
        return NULL;
    GfxShader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    size_t rn = strlen(res_json);
    uint8_t *buf = malloc(words * 4 + rn);
    s->spirv = malloc(words * 4), s->reflection = malloc(rn + 1);
    if (!buf || !s->spirv || !s->reflection) rt_fatal("gfx_shader: out of memory");
    memcpy(buf, spirv, words * 4), memcpy(buf + words * 4, res_json, rn);
    rt_sha256(buf, words * 4 + rn, s->key);
    free(buf);
    memcpy(s->spirv, spirv, words * 4), memcpy(s->reflection, res_json, rn + 1);
    s->words = words, s->reflection_size = rn;
    reflect(s);
    return s;
}

// ---- reflection: names of the .res.json values

static const char *const fmt_names[] = {   // img_format codes, in the translator's TextureFormat order
    NULL, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
    "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
    "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
    "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
    "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
    "R10G10B10A2Uint", "R11G11B10Float"};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

static const char *attr_name(uint8_t t) { return t == 1 ? "sint" : t == 2 ? "uint" : "float"; }

// ---- reflection: the .res.json the translator writes (tools/xlate/Program.cs Translate) and the SPIR-V itself

// `"name":` inside [o, e): the first byte after the colon and its spaces, NULL when absent.
static const char *json_key(const char *o, const char *e, const char *name) {
    char pat[40];
    int n = snprintf(pat, sizeof pat, "\"%s\":", name);
    for (const char *p = o; p + n <= e; p++)
        if (*p == '"' && !memcmp(p, pat, (size_t)n)) {
            p += n;
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
            return p;
        }
    return NULL;
}

static long json_int(const char *o, const char *e, const char *name, long def) {
    const char *p = json_key(o, e, name);
    return p ? strtol(p, NULL, 10) : def;
}

// Copies a string value into buf (empty when absent or not a string).
static void json_str(const char *o, const char *e, const char *name, char *buf, size_t n) {
    const char *p = json_key(o, e, name);
    size_t k = 0;
    if (p && *p == '"')
        for (p++; p < e && *p != '"' && k + 1 < n; p++) buf[k++] = *p;
    buf[k] = 0;
}

// The objects of the flat array `"name": [ {...}, ... ]`: calls fn for each object's [start, end). Returns the count.
static int json_objects(const char *js, const char *name, void (*fn)(GfxShader *, int, const char *, const char *),
                        GfxShader *s, int kind) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    if (!p || *p != '[') return 0;
    int n = 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{') continue;
        const char *q = memchr(p, '}', (size_t)(end - p));
        if (!q) break;
        if (fn) fn(s, kind, p, q);
        n++;
        p = q;
    }
    return n;
}

static int fmt_code(const char *name) {
    for (int i = 1; i < COUNT(fmt_names); i++)
        if (!strcmp(fmt_names[i], name)) return i;
    return 0;
}

// c2 slot of a texture or image handle (NVN's handle tables in c2: combined handles at 0x20, 32 slots; images at
// 0x120; separate textures at 0x168), -1 outside those ranges or for a handle in another constant buffer.
static int handle_slot(int kind, int cbuf_slot, int handle, int separate) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    int bytes = (handle & 0x3fff) * 4;
    if (kind == GFX_RES_IMG) return bytes >= 0x120 && bytes < 0x168 ? (bytes - 0x120) / 8 : -1;
    if (separate) return bytes >= 0x168 && bytes < 0x568 ? (bytes - 0x168) / 8 : -1;
    return bytes >= 0x20 && bytes < 0x120 ? (bytes - 0x20) / 8 : -1;
}

static void add_res(GfxShader *s, int kind, const char *o, const char *e) {
    GfxShaderRes *r = &s->res[kind][s->nres[kind]++];
    r->set = 0;
    r->binding = (uint16_t)json_int(o, e, "binding", 0);
    if (kind == GFX_RES_UBO || kind == GFX_RES_SSBO) {
        r->slot = (int16_t)json_int(o, e, "slot", -1);
        r->count = 1;
        if (kind == GFX_RES_SSBO) {
            r->cbuf_slot = (int8_t)json_int(o, e, "cb_slot", -1);
            r->handle = (uint16_t)json_int(o, e, "cb_offset", 0);
            r->flags = (uint8_t)json_int(o, e, "flags", 0);
        }
        return;
    }
    char buf[32];
    json_str(o, e, "format", buf, sizeof buf);
    const char *sep = json_key(o, e, "separate");
    r->type = (uint16_t)json_int(o, e, "type", 0);
    r->format = (uint8_t)fmt_code(buf);
    r->flags = (uint8_t)json_int(o, e, "flags", 0);
    r->cbuf_slot = (int8_t)json_int(o, e, "cbuf_slot", -1);
    int handle = (int)json_int(o, e, "handle", 0);
    r->handle = (uint16_t)handle;
    r->count = (uint16_t)json_int(o, e, "array", 1);
    r->separate = sep && *sep == 't';
    r->slot = (int16_t)handle_slot(kind, r->cbuf_slot, handle, r->separate);
}

// SPIR-V opcodes and enumerants used below (SPIR-V 1.6 specification, section 3).
enum { OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_ARRAY = 28, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71,
       DEC_BUILTIN = 11, DEC_LOCATION = 30, DEC_COMPONENT = 31, SC_INPUT = 1, SC_OUTPUT = 3, EM_LOCAL_SIZE = 17 };

// Interface Locations and the compute workgroup size from the module. Ids beyond 2^20 are ignored (the
// translator's modules stay far below).
static void reflect_spirv(GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    s->local[0] = s->local[1] = s->local[2] = 1;
    if (n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    // per id: location + 1 (0 = none), component flag, scalar/vector description of types, pointee of pointers
    int32_t *loc = calloc(bound, sizeof *loc);
    uint8_t *packed = calloc(bound, 1), *ttype = calloc(bound, 1), *tcomps = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee);
    if (!loc || !packed || !ttype || !tcomps || !pointee) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        uint32_t id = len > 1 ? a[0] : 0;
        if (op == OP_EXECUTION_MODE && len >= 6 && a[1] == EM_LOCAL_SIZE) {
            s->local[0] = a[2], s->local[1] = a[3], s->local[2] = a[4];
        } else if (op == OP_DECORATE && len >= 4 && id < bound) {
            if (a[1] == DEC_LOCATION) loc[id] = (int32_t)a[2] + 1;
            else if (a[1] == DEC_COMPONENT) packed[id] = 1;
        } else if (op == OP_TYPE_INT && len >= 4 && id < bound) {
            ttype[id] = a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT, tcomps[id] = 1;
        } else if (op == OP_TYPE_FLOAT && len >= 3 && id < bound) {
            ttype[id] = GFX_TYPE_FLOAT, tcomps[id] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = (uint8_t)a[2];
        } else if (op == OP_TYPE_ARRAY && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = tcomps[a[1]];   // element type (arrayed interface blocks)
        } else if (op == OP_TYPE_POINTER && len >= 4 && id < bound) {
            pointee[id] = a[2];
        } else if (op == OP_VARIABLE && len >= 4) {
            uint32_t type = a[0], var = a[1], sc = a[2];
            if ((sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound && loc[var] > 0 && loc[var] <= 32) {
                int l = loc[var] - 1;
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                GfxShaderIo io = {ttype[t], tcomps[t] ? tcomps[t] : 4, packed[var]};
                if (sc == SC_INPUT) s->in_mask |= 1u << l, s->in[l] = io;
                else s->out_mask |= 1u << l, s->out[l] = io;
            }
        }
        i += len;
    }
out:
    free(loc), free(packed), free(ttype), free(tcomps), free(pointee);
}

// Every gradient sample (OpImageSampleExplicitLod with Grad) whose two gradients are each built from two components gets
// them transposed, dx = (a, c) and dy = (b, d) from dx = (a, b) and dy = (c, d), by two new OpCompositeConstruct before
// it. Maxwell shaders fill a TXD's gradient registers per coordinate (du/dx, du/dy, dv/dx, dv/dy), and the translator
// (InstEmitTexture EmitTxd) builds dx = (first two registers), dy = (last two) = (du/dx, du/dy) and
// (dv/dx, dv/dy): a mip level chosen from the wrong derivatives, too sharp on terrain faces turned across the screen,
// which crawled in patches that followed the faces. MTF_TXD_TRANSPOSE=0 turns it off.
// Applied to the SPIR-V after it is loaded, so the shader cache is untouched.
enum { OP_COMPOSITE_CONSTRUCT = 80, OP_SAMPLE_EXPLICIT = 88, IO_GRAD = 4 };

static void patch_txd_transpose(GfxShader *s) {
    static int on = -1;
    static volatile int32_t n_logged;
    if (on < 0) {
        const char *e = cfg_get("MTF_TXD_TRANSPOSE");
        on = !(e && *e == '0');
    }
    uint32_t *w = s->spirv;
    uint64_t n = s->words;
    if (!on || n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    uint64_t *cc = calloc(bound, sizeof *cc);   // word index of a 2-constituent OpCompositeConstruct defining the id
    int found = 0;
    if (!cc) return;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto out;
        if (op == OP_COMPOSITE_CONSTRUCT && len == 5 && w[i + 2] < bound) cc[w[i + 2]] = i;
        if (op == OP_SAMPLE_EXPLICIT && len >= 8 && w[i + 5] == IO_GRAD && w[i + 6] < bound && w[i + 7] < bound &&
            cc[w[i + 6]] && cc[w[i + 7]] && w[cc[w[i + 6]] + 1] == w[cc[w[i + 7]] + 1])
            found++;
        i += len;
    }
    if (!found) goto out;
    uint32_t *o = malloc((n + (uint64_t)found * 10) * sizeof *o);
    if (!o) goto out;
    uint64_t k = 0;
    uint32_t next = w[3];
    for (uint64_t i = 0; i < 5; i++) o[k++] = w[i];
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OP_SAMPLE_EXPLICIT && len >= 8 && w[i + 5] == IO_GRAD && w[i + 6] < bound && w[i + 7] < bound &&
            cc[w[i + 6]] && cc[w[i + 7]] && w[cc[w[i + 6]] + 1] == w[cc[w[i + 7]] + 1]) {
            const uint32_t *dx = w + cc[w[i + 6]], *dy = w + cc[w[i + 7]];   // [hdr, type, id, a, b], [hdr, type, id, c, d]
            uint32_t ndx = next++, ndy = next++;
            o[k++] = 5u << 16 | OP_COMPOSITE_CONSTRUCT, o[k++] = dx[1], o[k++] = ndx, o[k++] = dx[3], o[k++] = dy[3];
            o[k++] = 5u << 16 | OP_COMPOSITE_CONSTRUCT, o[k++] = dx[1], o[k++] = ndy, o[k++] = dx[4], o[k++] = dy[4];
            for (uint32_t j = 0; j < len; j++) o[k++] = w[i + j];
            o[k - len + 6] = ndx, o[k - len + 7] = ndy;
        } else {
            for (uint32_t j = 0; j < len; j++) o[k++] = w[i + j];
        }
        i += len;
    }
    o[3] = next;
    free(s->spirv);
    s->spirv = o, s->words = k;
    if (plat_inc32(&n_logged) <= 4) rt_log("gfx_shader: %d gradient samples now read their derivatives per coordinate", found);
out:
    free(cc);
}

static void reflect(GfxShader *s) {
    static const char *const kinds[GFX_RES_KINDS] = {"cbufs", "sbufs", "textures", "images"};
    const char *js = s->reflection;
    char stage[8];
    json_str(js, js + strlen(js), "stage", stage, sizeof stage);
    s->stage = !strcmp(stage, "cs") ? GFX_STAGE_CS : !strcmp(stage, "ps") ? GFX_STAGE_FS : !strcmp(stage, "gs") ? GFX_STAGE_GS
             : !strcmp(stage, "hs") ? GFX_STAGE_TCS : !strcmp(stage, "ds") ? GFX_STAGE_TES : GFX_STAGE_VS;
    s->mvprev_banks = (uint32_t)json_int(js, js + strlen(js), "mvprev_banks", 0);
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        int count = json_objects(js, kinds[k], NULL, s, k);
        s->res[k] = calloc(count ? count : 1, sizeof *s->res[k]);
        json_objects(js, kinds[k], add_res, s, k);
    }
    reflect_spirv(s);
    patch_txd_transpose(s);
}

// ---- use masks

// The slots of the int array `"name": [...]` below 32 as a mask.
static uint32_t json_slot_mask(const char *js, const char *name) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    uint32_t m = 0;
    if (!p || *p != '[') return 0;
    for (p++; p < end && *p != ']';) {
        char *q;
        long v = strtol(p, &q, 10);
        if (q == p) {
            p++;
            continue;
        }
        if (v >= 0 && v < 32) m |= 1u << v;
        p = q;
    }
    return m;
}

static UseMask use_of(const GfxShader *s) {
    UseMask m = {json_slot_mask(s->reflection, "tex_queried") | json_slot_mask(s->reflection, "img_queried"), 0, 0};
    if (s->stage == GFX_STAGE_VS) m.attrs = s->in_mask;
    else if (s->stage == GFX_STAGE_FS) m.fouts = s->out_mask & 0xff;
    return m;
}

// ---- NVN2 (Switch 2) shaders, prebuilt: <dir>/<key>.spv and <key>.res.json (tools/shaders/translate.py). Nothing is
// translated at run time. The directory: MTF_SHADER_DIR, else shaders/ beside mtf-recomp.exe (an installed folder),
// else ../generated/shaders/spv from it (a developer build in build/bin).
static char nvn2_dir[PATH_CAP];
static PlatOnce nvn2_once = PLAT_ONCE_INIT;
static volatile int32_t n_nvn2_loaded, n_nvn2_missing;

static void nvn2_init(void *unused) {
    (void)unused;
    const char *e = cfg_get("MTF_SHADER_DIR");
    if (e && *e) snprintf(nvn2_dir, sizeof nvn2_dir, "%s", e);
    else {
        char exe[PLAT_PATH_MAX];
        int len = plat_exe_path(exe, sizeof exe);
        char *cut = len && len < (int)sizeof exe ? plat_last_sep(exe) : NULL;
        if (cut) *cut = 0;
        snprintf(nvn2_dir, sizeof nvn2_dir, "%s/shaders", cut ? exe : ".");
        if (!plat_dir_exists(nvn2_dir)) snprintf(nvn2_dir, sizeof nvn2_dir, "%s/../generated/shaders/spv", cut ? exe : ".");
    }
    rt_log("gfx_shader: NVN2 shaders from %s (prebuilt; MTF_SHADER_DIR overrides)", nvn2_dir);
}

// A prebuilt NVN2 shader declared an input or output with another type than the pipeline gives it (an integer vertex
// attribute read as float: bone indices). Prebuilt shaders cannot be translated again here, so the answer is written
// once to <shader dir>/../answers.tsv, which tools/shaders/translate.py --answers takes to translate them again.
void gfx_shader_answer_note(const GfxShader *s, int fout, int slot, int type) {
    if (!s || memcmp(s->key + 8, "NVN2 prebuilt", 13) || slot < 0 || slot > 31) return;
    static struct { uint64_t key; uint32_t what; } seen[4096];
    static int nseen;
    static PlatMutex lock = PLAT_MUTEX_INIT;
    uint64_t key;
    memcpy(&key, s->key, 8);
    uint32_t what = (uint32_t)fout << 16 | (uint32_t)slot << 8 | (uint32_t)type;
    plat_mutex_lock(&lock);
    for (int i = 0; i < nseen; i++)
        if (seen[i].key == key && seen[i].what == what) {
            plat_mutex_unlock(&lock);
            return;
        }
    if (nseen < (int)(sizeof seen / sizeof *seen)) seen[nseen].key = key, seen[nseen].what = what, nseen++;
    plat_once(&nvn2_once, nvn2_init, NULL);
    char hex[17], path[PATH_CAP + 32];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", s->key[i]);
    snprintf(path, sizeof path, "%s/../answers.tsv", nvn2_dir);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\t%s\t%d\t%s\n", hex, fout ? "fout" : "attr", slot, attr_name((uint8_t)type));
        fclose(f);
    }
    static volatile int32_t n;
    if (plat_inc32(&n) <= 8)
        rt_log("[RED] gfx_shader: NVN2 shader %s %s %d is %s in the pipeline, not as translated: noted in %s "
               "(tools/shaders/translate.py --answers)", hex, fout ? "output" : "input", slot,
               attr_name((uint8_t)type), path);
    plat_mutex_unlock(&lock);
}

// The translator leaves the driver constant segments (c0, c2) out of the reflection: the host fills them from
// the bindings it keeps on the side. A module that reads one declares a uniform block named
// <stage>_c<bank> (cp_c0: compute's c0, 60 of this game's compute shaders, 7 vertex and 2 pixel shaders); each such
// block whose binding the reflection lacks becomes a uniform buffer of that bank, which this runtime fills (c0_info).
static void nvn2_driver_banks(GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    uint32_t bound = n >= 5 && w[3] < (1u << 20) ? w[3] : 0;
    int16_t *bank = bound ? malloc(bound * sizeof *bank) : NULL;
    int32_t *binding = bound ? malloc(bound * sizeof *binding) : NULL;
    if (!bank || !binding) goto out;
    for (uint32_t i = 0; i < bound; i++) bank[i] = -1, binding[i] = -1;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == 5 && len >= 3 && w[i + 1] < bound) {   // OpName
            const char *name = (const char *)(w + i + 2);
            size_t max = (len - 2) * 4, l = strnlen(name, max);
            if (l >= 5 && l <= 7 && name[1] == 'p' && name[2] == '_' && name[3] == 'c' && isdigit((unsigned char)name[4])) {
                int b = atoi(name + 4);
                if (b == 0 || b == 2) bank[w[i + 1]] = (int16_t)b;
            }
        } else if (op == OP_DECORATE && len >= 4 && w[i + 2] == 33 && w[i + 1] < bound) {   // Binding
            binding[w[i + 1]] = (int32_t)w[i + 3];
        }
        i += len;
    }
    for (uint32_t id = 0; id < bound; id++) {
        if (bank[id] < 0 || binding[id] < 1) continue;
        int have = 0;
        for (int k = 0; k < GFX_RES_KINDS && !have; k++)
            for (int j = 0; j < s->nres[k]; j++)
                if (s->res[k][j].binding == (uint16_t)binding[id]) have = 1;
        if (have) continue;
        GfxShaderRes *r = realloc(s->res[GFX_RES_UBO], (size_t)(s->nres[GFX_RES_UBO] + 1) * sizeof *r);
        if (!r) rt_fatal("gfx_shader: out of memory");
        s->res[GFX_RES_UBO] = r;
        r[s->nres[GFX_RES_UBO]++] = (GfxShaderRes){.binding = (uint16_t)binding[id], .slot = bank[id], .count = 1,
                                                   .cbuf_slot = -1};
    }
out:
    free(bank), free(binding);
}

// The reflection and SPIR-V of a prebuilt shader, checked; want: its stage's name in the reflection, NULL for any. 0, with
// nothing to free, when one is missing or wrong.
static int nvn2_read(const char *hex, const char *variant, const char *want, uint8_t **res, uint64_t *res_size, uint8_t **spv,
                     uint64_t *spv_size) {
    char p[PATH_CAP + 40];
    snprintf(p, sizeof p, "%s/%s%s.res.json", nvn2_dir, hex, variant);
    *res = read_file(p, res_size);
    snprintf(p, sizeof p, "%s/%s%s.spv", nvn2_dir, hex, variant);
    *spv = *res ? read_file(p, spv_size) : NULL;
    uint32_t magic = 0;
    if (*spv && *spv_size >= 20) memcpy(&magic, *spv, 4);
    const char *st = *res ? strstr((const char *)*res, "\"stage\"") : NULL;
    st = st ? strchr(st + 7, '"') : NULL;
    if (*res && reflection_valid((const char *)*res, *res_size) && *spv && !(*spv_size % 4) && magic == 0x07230203 &&
        spirv_complete((const uint32_t *)*spv, *spv_size / 4) && st && (!want || !strncmp(st + 1, want, 2)))
        return 1;
    free(*res), free(*spv);
    return 0;
}

static void ensure_storage_caps(uint32_t **spirv, uint64_t *words) {
    const uint32_t *w = *spirv;
    uint64_t n = *words;
    if (!w || n < 5) return;
    int has_rcap = 0, has_wcap = 0, has_read = 0, has_write = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return;
        if (op == 17 && len == 2) {
            if (w[i + 1] == 55) has_rcap = 1;   // OpCapability StorageImageReadWithoutFormat
            else if (w[i + 1] == 56) has_wcap = 1;   // OpCapability StorageImageWriteWithoutFormat
        } else if (op == 98) has_read = 1;      // OpImageRead
        else if (op == 99) has_write = 1;       // OpImageWrite
        i += len;
    }
    uint32_t add = (uint32_t)((has_read && !has_rcap ? 1 : 0) + (has_write && !has_wcap ? 1 : 0));
    if (!add) return;
    uint32_t *out = malloc((size_t)(n + add * 2) * 4);
    if (!out) return;
    memcpy(out, w, 5 * 4);
    uint64_t k = 5;
    if (has_read && !has_rcap) { out[k++] = (2u << 16) | 17u; out[k++] = 55u; }
    if (has_write && !has_wcap) { out[k++] = (2u << 16) | 17u; out[k++] = 56u; }
    memcpy(out + k, w + 5, (size_t)(n - 5) * 4);
    free(*spirv);
    *spirv = out;
    *words = n + add * 2;
}

// The shader object of files nvn2_read checked (it takes them), entered in the table; NULL when the replay cannot bind it.
static GfxShader *nvn2_make(const uint8_t key[32], const char *hex, const char *want, int stage, uint8_t *res, uint64_t res_size,
                            uint8_t *spv, uint64_t spv_size) {
    GfxShader *s = calloc(1, sizeof *s);
    if (!s) rt_fatal("gfx_shader: out of memory");
    memcpy(s->key, key, 32);
    memcpy(s->iid, key, 32);
    s->spirv = (uint32_t *)spv;
    s->words = spv_size / 4;
    ensure_storage_caps(&s->spirv, &s->words);
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    reflect(s);
    nvn2_driver_banks(s);
    uint32_t descriptors = 0;
    for (int k = 0; k < GFX_RES_KINDS; k++)
        for (int j = 0; j < s->nres[k]; j++) descriptors += s->res[k][j].count ? s->res[k][j].count : 1;
    if (descriptors > 1024) {   // vk_exec_draw.c MAX_DESC. 3032440e5027bbe5 (shader/audience/audience_compute) indexed
        // 8192 handles of NVN2's per-shader c[0] by its workgroup until the translator sized c[0] arrays as c[2]'s 301
        // handle words (tools/shaders/dmsh_xlate); skipped, the crowd flickered
        rt_log("[RED] gfx_shader: NVN2 %s shader %s has %u descriptors, more than the replay binds: its %s skipped "
               "(open)", want, hex, descriptors, stage == GFX_STAGE_CS ? "dispatches are" : "draws are");
        shader_free(s);
        return NULL;
    }
    if (s->stage == GFX_STAGE_FS) vk_hdr_shader_swap(&s->spirv, &s->words);
    int32_t n = plat_inc32(&n_nvn2_loaded);
    static int trace = -1;
    if (trace < 0) trace = cfg_bool("MTF_SHADER_TRACE");
    if (trace || !(n & (n - 1))) rt_log("gfx_shader: %d prebuilt NVN2 shaders loaded (latest %s, %s)", n, hex, want);
    return mem_insert(s);
}

static const GfxShader *nvn2_get(int stage, const uint8_t *control, const uint8_t *code, int mvprev) {
    uint8_t *state = (uint8_t *)control + NVN2_CTL_COPY;   // the program's copy (nvn_api_obj.c ctl_copy)
    uint32_t len = nvn_ctl_len(control);
    if (state[0] == 2) return NULL;
    if (!code || !len) {
        static volatile int32_t n;
        if (plat_inc32(&n) <= 16)
            rt_log("[RED] gfx_shader: NVN2 %s shader with code %p, length %u: not loaded (stage byte %u)",
                   stage == GFX_STAGE_VS ? "vs" : stage == GFX_STAGE_FS ? "ps" : stage == GFX_STAGE_GS ? "gs"
                   : stage == GFX_STAGE_TCS ? "hs" : stage == GFX_STAGE_TES ? "ds" : "cs",
                   (const void *)code, len,
                   control[0x56]);
        return NULL;
    }
    if (!state[0]) {
        uint8_t ctl[NVN2_KEY_CTL], md[32];
        memcpy(ctl, control, sizeof ctl);
        memset(ctl + 0x30, 0, 4);
        Sha256 h;
        sha256_init(&h);
        sha256_update(&h, ctl, sizeof ctl);
        sha256_update(&h, code, len);
        sha256_final(&h, md);
        memcpy(state + 1, md, 8);
        state[0] = 1;
    }
    uint8_t key[32] = {0};
    memcpy(key, state + 1, 8);
    memcpy(key + 8, mvprev ? "NVN2 mvprev  " : "NVN2 prebuilt", 13);
    GfxShader *s = mem_find(key);
    if (s) return plat_inc64(&n_hits), s;
    plat_once(&nvn2_once, nvn2_init, NULL);
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", state[1 + i]);
    // gs: the layer-picking geometry shaders; hs and ds: the tessellation stages (grass courts, sea of clouds)
    const char *want = stage == GFX_STAGE_VS ? "vs" : stage == GFX_STAGE_FS ? "ps" : stage == GFX_STAGE_GS ? "gs"
                     : stage == GFX_STAGE_TCS ? "hs" : stage == GFX_STAGE_TES ? "ds" : "cs";
    uint64_t res_size = 0, spv_size = 0;
    uint8_t *res, *spv;
    const char *variant = mvprev ? ".mvprev" : "";   // a vertex shader's last-frame variant (translate.py --mvprev)
    if (!nvn2_read(hex, variant, want, &res, &res_size, &spv, &spv_size)) {
        int32_t n = plat_inc32(&n_nvn2_missing);
        if (n <= 16 || !(n & (n - 1)))
            rt_log("[RED] gfx_shader: no prebuilt NVN2 %s shader %s%s in %s (%d so far): its draws are skipped", want, hex,
                   variant, nvn2_dir, n);
        if (!mvprev) state[0] = 2;
        plat_inc64(&n_failures);
        return NULL;
    }
    s = nvn2_make(key, hex, want, stage, res, res_size, spv, spv_size);
    if (!s) {
        state[0] = 2;
        plat_inc64(&n_failures);
    }
    return s;
}

// A shader the game hands NVN, by its NVN2 control section: the prebuilt SPIR-V of nvn2_get. Every shader of this game
// is NVN2 (the Switch 2's format); a Switch 1 (NVN1) section has no translator here and gets nothing.
static const GfxShader *shader_by_control(int stage, const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a) {
    if (control && nvn2_ctl(control)) return nvn2_get(stage, control, code, a && a->mvprev);
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 4) rt_log("[RED] gfx_shader: a shader without an NVN2 control section: not supported");
    return NULL;
}

const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a) {
    (void)code_size, (void)c1;
    return shader_by_control(stage, control, code, a);
}

// Prebuilt shaders are ready at once: never pending.
const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending) {
    (void)code_size;
    *pending = 0;
    return shader_by_control(stage, control, code, a);
}

const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a) {
    (void)code_size;
    return shader_by_control(stage, control, code, a);
}

// ---- boot prewarm (vk_pipeline_prewarm): an input id is the key of nvn2_get, so a shader recorded in an earlier run is found
// again by name, without the game's control section
typedef struct {
    const uint8_t (*iids)[32];
    size_t n;
    const GfxShader **out;
    volatile int64_t next;
    volatile int32_t missing;
} Warm;

static const GfxShader *nvn2_load_id(const uint8_t iid[32]) {
    const GfxShader *known = mem_find(iid);
    if (known) return known;
    int mvprev = !memcmp(iid + 8, "NVN2 mvprev  ", 13);
    if (!mvprev && memcmp(iid + 8, "NVN2 prebuilt", 13)) return NULL;
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", iid[i]);
    uint8_t *res, *spv;
    uint64_t res_size = 0, spv_size = 0;
    if (!nvn2_read(hex, mvprev ? ".mvprev" : "", NULL, &res, &res_size, &spv, &spv_size)) return NULL;
    return nvn2_make(iid, hex, "recorded", -1, res, res_size, spv, spv_size);
}

static void warm_worker(void *arg) {
    Warm *w = arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&w->next) - 1;
        if (i >= w->n) return;
        if (!(w->out[i] = nvn2_load_id(w->iids[i]))) plat_inc32(&w->missing);
    }
}

void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads) {
    plat_once(&nvn2_once, nvn2_init, NULL);
    uint64_t t0 = plat_now_ms();
    memset(out, 0, n * sizeof *out);
    Warm w = {.iids = iids, .n = n, .out = out};
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    if ((size_t)threads > n) threads = n ? (int)n : 1;
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(warm_worker, &w, "shader prewarm"))) k++;
    if (!k) warm_worker(&w);
    for (int i = 0; i < k; i++) plat_thread_join(t[i]);
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys (%ld not there) on %d threads in %llu ms", n, (long)w.missing, k,
           (unsigned long long)(plat_now_ms() - t0));
}

// ---- accessors (gfx.h)

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words) {
    if (words) *words = s->words;
    return s->spirv;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

// The c2 slots whose texture and image types the shader's translation read (.res.json tex_queried, img_queried): the
// replay compares only those when it looks for the shader's variant.
uint32_t gfx_shader_slot_mask(const GfxShader *s) { return use_of(s).slots; }

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}

uint32_t gfx_shader_mvprev_banks(const GfxShader *s) { return s->mvprev_banks; }

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}
