// nn::codec video decoders (the Switch 2 SDK) on the host: VP8 and VP9 through libvpx. The SDK sends every decoder call
// to a decoder service over IPC that this host does not run (nn::codec::video::detail::ipc::Initialize stopped the
// boot), so the whole API is taken over. MTF's movies are VP9 WebM (with Vorbis audio, decoded by the
// game's own code); its movie::VideoDecoder drives these calls.
//
// The protocol: the client makes one image pool per picture (MakeDecodeImagePreference, CalculateDecoderImagePoolBuffer,
// InitializeDecoderImagePool) and registers each with the decoder under an index; a registered index is acquirable at
// once with an already-signalled sync handle. Acquire takes the oldest index; Release gives the buffer to the decoder
// to fill and makes it acquirable again with a new sync handle, which signals once a picture is in it. The client then
// polls nn::gs::TryWaitNativeSync (or a nn::syncpt event), reads the properties and the picture (RGBA or NV12).
// Submit takes one packet; 3603 means "cannot take more now" and the client retries after the submission-ready event.
// H.264 and AV1 are not served (the game ships none): their Initialize is a named fatal.
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

#define CV_MAGIC 0x56436d44u        // 'DmCV'
#define CV_SYNC 0x53436d44u         // 'DmCS': a sync handle the host made (decoder id, index, generation)
#define CV_BUFS 32
#define CV_FIFO 8
#define CV_DECODERS 16
#define CV_READY 4                  // held pictures at which the submission-ready event goes down
#define CV_HOLD 12                  // and at which packets are refused. The game's player sends one packet per signal and
                                    // ignores the result, so a signal in flight when the event goes down must still find
                                    // room (refused at 4, a packet of a movie was lost)
#define CV_EVENT_OFF 0x100          // the submission-ready SystemEventType inside the decoder type
#define CV_R(desc) ((uint32_t)(0x6f | ((desc) << 9)))   // nn::codec results (module 111)
#define CV_BUSY CV_R(3603)
#define CV_NONE CV_R(3604)
#define CV_BAD CV_R(2004)
#define NATIVE_ALIGN 0x4000ull
#define SPT_OFF 0x40                // the host's record inside a nn::syncpt::sdk::SyncPointEventType
#define SPT_MAX 64

typedef struct { uint16_t w, h; uint32_t fmt, mode, magic; uint32_t pad[2]; } CvPref;   // VideoDecoderImagePreference
typedef struct { uint32_t magic, dec, idx, gen; } CvSync;                               // nn::gs::NativeSyncHandle
typedef struct {
    uint32_t magic;
    uint16_t w, h;
    uint32_t fmt, count;
    vpx_image_t **img;
    uint8_t *mem;           // the game's pool memory
    uint64_t size;
    int slot;               // its place in pools[] + 1, once a native image handle was asked for
    volatile int native;    // a handle was asked for: each picture is also kept as RGBA in mem
} CvPool;
typedef struct {
    CvPool *pool;
    uint32_t pidx, gen, filled_gen;
    int reg, filled;
    uint16_t w, h;
    int64_t ts;
    uint8_t eos;
} CvBuf;
typedef struct {
    uint32_t magic, id;
    PlatMutex mu;
    vpx_codec_ctx_t vpx;
    int vpx_ok, vp8;
    uint16_t w, h;
    uint64_t event;         // guest SystemEventType
    uint64_t submits, pictures;
    CvBuf buf[CV_BUFS];
    uint32_t fifo[CV_FIFO], fhead, fcount;      // acquirable indices
    uint32_t pend[CV_BUFS], phead, pcount;      // released, waiting for a picture
    vpx_image_t *held[CV_HOLD];
    int64_t held_ts[CV_HOLD];
    uint8_t held_eos[CV_HOLD];
    int nheld, eos_pending;
} CvDec;
typedef struct { uint32_t magic, armed; CvSync want; } CvSpt;

static CvDec *decoders[CV_DECODERS];
static CvPool *pools[256];
static uint64_t armed[SPT_MAX];                 // armed syncpt events (guest addresses)
static PlatMutex g_mu = PLAT_MUTEX_INIT;        // lock order: g_mu, a decoder's mu, spt_mu
static PlatMutex spt_mu = PLAT_MUTEX_INIT;

// MTF_CODEC_TRACE=1: the first calls of the decoder protocol, one line each
static void ctrace(const char *fmt, ...) {
    static int on = -1;
    static volatile int32_t n;
    if (on < 0) on = cfg_bool("MTF_CODEC_TRACE");
    if (!on || plat_inc32(&n) > 400) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("codec trace: %s", buf);
}

static uint64_t sdk_sym(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("nn::codec: the SDK does not define %s", name);
    return a;
}

static CvDec *dec_of(uint64_t t) {
    CvDec *d = t ? *(CvDec **)(uintptr_t)t : NULL;
    return d && d->magic == CV_MAGIC ? d : NULL;
}
static CvPool *pool_of(uint64_t t) {
    CvPool *p = t ? *(CvPool **)(uintptr_t)t : NULL;
    return p && p->magic == CV_MAGIC ? p : NULL;
}
static CvSpt *spt_of(uint64_t e) { return (CvSpt *)(uintptr_t)(e + SPT_OFF); }

// ---- pictures
// Retired pictures wait here for the next copy of their size. A 1440p copy from the heap is a new mapping each time:
// 1350 page faults a picture, which at 60 pictures a second was most of a core in the kernel during the opening movie.
#define CV_SPARE 16
static vpx_image_t *spare[CV_SPARE];
static int nspare;
static PlatMutex spare_mu = PLAT_MUTEX_INIT;

static void img_retire(vpx_image_t *im) {
    plat_mutex_lock(&spare_mu);
    if (nspare < CV_SPARE) spare[nspare++] = im, im = NULL;
    plat_mutex_unlock(&spare_mu);
    if (im) vpx_img_free(im);
}

static vpx_image_t *img_take(unsigned w, unsigned h) {
    vpx_image_t *c = NULL;
    plat_mutex_lock(&spare_mu);
    for (int i = nspare - 1; i >= 0 && !c; i--)
        if (spare[i]->d_w == w && spare[i]->d_h == h) c = spare[i], spare[i] = spare[--nspare];
    plat_mutex_unlock(&spare_mu);
    return c ? c : vpx_img_alloc(NULL, VPX_IMG_FMT_I420, w, h, 1);
}

static void img_spares_free(void) {   // a decoder ended: the next movie may be another size, and none may come
    plat_mutex_lock(&spare_mu);
    while (nspare) vpx_img_free(spare[--nspare]);
    plat_mutex_unlock(&spare_mu);
}

static vpx_image_t *img_copy(const vpx_image_t *im) {   // an owned I420 copy (libvpx reuses its buffers)
    vpx_image_t *c = img_take(im->d_w, im->d_h);
    if (!c) return NULL;
    c->cs = im->cs;
    c->range = im->range;
    for (int p = 0; p < 3; p++) {
        unsigned w = p ? (im->d_w + 1) / 2 : im->d_w, h = p ? (im->d_h + 1) / 2 : im->d_h;
        for (unsigned y = 0; y < h; y++)
            memcpy(c->planes[p] + (size_t)y * c->stride[p], im->planes[p] + (size_t)y * im->stride[p], w);
    }
    return c;
}

// I420 to NV12 (Y stride w, h rows, then interleaved UV); a smaller picture is padded black, a larger one cropped
static void to_nv12(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) {
        memset(dst, 16, (size_t)w * h);
        memset(dst + (size_t)w * h, 128, (size_t)w * h / 2);
    }
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * w, im->planes[0] + (size_t)y * im->stride[0], (size_t)cw);
    int cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    for (int y = 0; y < ch2 && y < h / 2; y++) {
        const uint8_t *u = im->planes[1] + (size_t)y * im->stride[1], *v = im->planes[2] + (size_t)y * im->stride[2];
        uint8_t *o = dst + (size_t)w * h + (size_t)y * w;
        for (int x = 0; x < cw2 && 2 * x + 1 < w; x++) {
            o[2 * x] = u[x];
            o[2 * x + 1] = v[x];
        }
    }
}

// I420 to RGBA rows of `stride` bytes, by the colour space and range the stream declares (undeclared: BT.709 from 720
// lines up, else BT.601), 16-bit fixed point
static void to_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h, size_t stride) {
    int full = im->range == VPX_CR_FULL_RANGE, b709 = im->cs == VPX_CS_BT_709 || (im->cs == VPX_CS_UNKNOWN && h >= 720);
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536),
        BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) ((uint32_t *)(dst + (size_t)y * stride))[x] = 0xff000000u;
#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = im->planes[0] + (size_t)y * im->stride[0];
        const uint8_t *pu = im->planes[1] + (size_t)(y >> 1) * im->stride[1];
        const uint8_t *pv = im->planes[2] + (size_t)(y >> 1) * im->stride[2];
        uint8_t *o = dst + (size_t)y * stride;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r);
            o[4 * x + 1] = (uint8_t)CL(g);
            o[4 * x + 2] = (uint8_t)CL(b);
            o[4 * x + 3] = 255;
        }
    }
#undef CL
}

// A native image: one picture's RGBA in the pool's memory, rows on the 64-byte stride NVN's linear textures take, each
// picture on a 16 KB boundary
static uint32_t native_stride(uint32_t w) { return (w * 4 + 63) & ~63u; }
static uint64_t native_size(uint32_t w, uint32_t h) {
    return ((uint64_t)native_stride(w) * h + NATIVE_ALIGN - 1) & ~(NATIVE_ALIGN - 1);
}
static void pool_native_write(CvPool *p, uint32_t i, const vpx_image_t *im) {
    if (!p->native || !p->mem || (i + 1) * native_size(p->w, p->h) > p->size) return;
    to_rgba(im, p->mem + i * native_size(p->w, p->h), p->w, p->h, native_stride(p->w));
}

// ---- sync point events: the event type holds a SystemEventType at +0 (bound by the host) and the awaited handle at
// +0x40; an armed one is signalled when its buffer is filled
static int sync_reached(const CvSync *s);
static void spt_fire(CvDec *d, uint32_t i) {   // buffer i of d was filled (decoder lock held)
    plat_mutex_lock(&spt_mu);
    for (int k = 0; k < SPT_MAX; k++) {
        CvSpt *t = armed[k] ? spt_of(armed[k]) : NULL;
        if (t && t->want.dec == d->id && t->want.idx == i && d->buf[i].filled_gen >= t->want.gen) {
            t->armed = 0;
            kern_signal_system_event(armed[k]);
            armed[k] = 0;
        }
    }
    plat_mutex_unlock(&spt_mu);
}
static void spt_unarm(uint64_t e) {
    plat_mutex_lock(&spt_mu);
    for (int k = 0; k < SPT_MAX; k++)
        if (armed[k] == e) armed[k] = 0;
    plat_mutex_unlock(&spt_mu);
    spt_of(e)->armed = 0;
}

// Held pictures and a pending end of stream into the released buffers, in order (decoder lock held). The event rises
// when a packet was taken (took) or room came back, not at every release: the player counts each signal as one packet
// it may send.
static void drain(CvDec *d, int took) {
    int before = d->nheld;
    while (d->pcount && (d->nheld || d->eos_pending)) {
        uint32_t i = d->pend[d->phead];
        d->phead = (d->phead + 1) % CV_BUFS;
        d->pcount--;
        CvBuf *b = &d->buf[i];
        if (d->nheld) {
            vpx_image_t *im = d->held[0];
            b->ts = d->held_ts[0];
            b->eos = d->held_eos[0];
            memmove(d->held, d->held + 1, sizeof d->held[0] * (CV_HOLD - 1));
            memmove(d->held_ts, d->held_ts + 1, sizeof d->held_ts[0] * (CV_HOLD - 1));
            memmove(d->held_eos, d->held_eos + 1, CV_HOLD - 1);
            d->nheld--;
            b->w = (uint16_t)im->d_w;
            b->h = (uint16_t)im->d_h;
            if (b->pool && b->pidx < b->pool->count) {
                pool_native_write(b->pool, b->pidx, im);
                if (b->pool->img[b->pidx]) img_retire(b->pool->img[b->pidx]);
                b->pool->img[b->pidx] = im;
            } else {
                img_retire(im);
            }
            if (!d->pictures++)
                rt_log("codec: %s decoder %u: first picture %ux%u into buffer %u", d->vp8 ? "VP8" : "VP9", d->id, b->w,
                       b->h, i);
            if (cfg_bool("MTF_CODEC_TRACE") && (d->pictures == 30 || d->pictures == 300 || d->pictures == 900)) {
                // a decoded picture as a PPM next to the log, at a quarter of its size
                int w = (int)im->d_w / 4, h = (int)im->d_h / 4;
                uint8_t *rgba = malloc((size_t)im->d_w * im->d_h * 4);
                char path[64];
                snprintf(path, sizeof path, "codec-picture-%llu.ppm", (unsigned long long)d->pictures);
                FILE *f = rgba ? fopen(path, "wb") : NULL;
                if (f) {
                    to_rgba(im, rgba, (int)im->d_w, (int)im->d_h, (size_t)im->d_w * 4);
                    fprintf(f, "P6 %d %d 255\n", w, h);
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) fwrite(rgba + ((size_t)y * 4 * im->d_w + (size_t)x * 4) * 4, 1, 3, f);
                    fclose(f);
                    rt_log("codec trace: picture %llu written to %s", (unsigned long long)d->pictures, path);
                }
                free(rgba);
            }
            if (!(d->pictures % 600))
                rt_log("codec: %s decoder %u: %llu pictures, %llu packets", d->vp8 ? "VP8" : "VP9", d->id,
                       (unsigned long long)d->pictures, (unsigned long long)d->submits);
        } else {
            b->w = b->h = 0;
            b->ts = 0;
            b->eos = 1;
            d->eos_pending = 0;
        }
        if (b->eos)
            rt_log("codec: %s decoder %u: end of stream after %llu pictures", d->vp8 ? "VP8" : "VP9", d->id,
                   (unsigned long long)d->pictures);
        b->filled = 1;
        b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (d->event) {
        if (d->nheld >= CV_READY) kern_clear_system_event(d->event);
        else if (took || before >= CV_READY) kern_signal_system_event(d->event);
    }
}

static void vpx_open(CvDec *d) {   // (re)creates the codec state: a new stream starts at the next key frame
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = d->w, .h = d->h};
    d->vpx_ok = vpx_codec_dec_init(&d->vpx, d->vp8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
    if (!d->vpx_ok) rt_log("[RED] codec: %s decoder: libvpx init failed", d->vp8 ? "VP8" : "VP9");
}

// ---- nn::codec::video and the image pools
static void ov_video_init(Cpu *c) { ret_x(c, 0); }   // the decoder service is the host's
static void ov_video_fin(Cpu *c) { ret_x(c, 0); }

// VideoDecoderImagePreference MakeDecodeImagePreference(u16 w, u16 h, VideoDecoderOutputFormat, CodecMode): returned
// in memory at x8 (24 bytes)
static void ov_make_pref(Cpu *c) {
    CvPref *out = (CvPref *)(uintptr_t)c->x[8];
    *out = (CvPref){(uint16_t)arg_x(c, 0), (uint16_t)arg_x(c, 1), (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 3), CV_MAGIC,
                    {0, 0}};
}

// Result CalculateDecoderImagePoolBuffer(size_t *size, size_t *align, const VideoDecoderImagePreference &, u32 count)
static void ov_pool_size(Cpu *c) {
    uint64_t *size = arg_p(c, 0), *align = arg_p(c, 1);
    const CvPref *p = arg_p(c, 2);
    uint32_t n = (uint32_t)arg_x(c, 3);
    *size = native_size(p->w, p->h) * (n ? n : 1);
    *align = NATIVE_ALIGN;
    ret_x(c, 0);
}

// Result InitializeDecoderImagePool(VideoDecoderImagePoolType *, void *mem, size_t, const Preference &, u32, CodecMode)
static void ov_pool_init(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    const CvPref *p = arg_p(c, 3);
    uint32_t n = (uint32_t)arg_x(c, 4);
    CvPool *o = calloc(1, sizeof *o);
    if (!o) rt_fatal("codec: image pool allocation");
    o->magic = CV_MAGIC;
    o->w = p->w;
    o->h = p->h;
    o->fmt = p->fmt;
    o->count = n ? n : 1;
    o->img = calloc(o->count, sizeof *o->img);
    o->mem = arg_p(c, 1);
    o->size = arg_x(c, 2);
    *(CvPool **)(uintptr_t)t = o;
    static int once;
    if (!once++)
        rt_log("codec: image pool %ux%u format %u, mode %u, %u picture(s): the host keeps the decoded pictures", p->w, p->h,
               p->fmt, p->mode, o->count);
    ret_x(c, 0);
}

// void FinalizeDecoderImagePool(VideoDecoderImagePoolType *)
static void ov_pool_fin(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    CvPool *p = pool_of(t);
    if (!p) return;
    plat_mutex_lock(&g_mu);   // a decoder still holding a buffer of this pool keeps it, with no pool to write into
    for (int k = 0; k < CV_DECODERS; k++) {
        CvDec *d = decoders[k];
        if (!d) continue;
        plat_mutex_lock(&d->mu);
        for (int i = 0; i < CV_BUFS; i++)
            if (d->buf[i].pool == p) d->buf[i].pool = NULL;
        plat_mutex_unlock(&d->mu);
    }
    if (p->slot) pools[p->slot - 1] = NULL;
    plat_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < p->count; i++)
        if (p->img[i]) vpx_img_free(p->img[i]);
    free(p->img);
    p->magic = 0;
    free(p);
    *(CvPool **)(uintptr_t)t = NULL;
}

// u32 GetDecoderImagePoolNativeImageHandle(const VideoDecoderImagePoolType *, u32 index): (slot + 1) << 8 | index;
// the pool keeps its pictures as RGBA from now on
static void ov_pool_native(Cpu *c) {
    CvPool *p = pool_of(arg_x(c, 0));
    uint32_t i = (uint32_t)arg_x(c, 1);
    if (!p || i >= p->count) { ret_x(c, 0); return; }
    plat_mutex_lock(&g_mu);
    if (!p->slot)
        for (int k = 0; k < 256; k++)
            if (!pools[k]) {
                pools[k] = p;
                p->slot = k + 1;
                break;
            }
    plat_mutex_unlock(&g_mu);
    if (!p->slot) rt_fatal("codec: more than 256 image pools with native images");
    p->native = 1;
    static int once;
    if (!once++) rt_log("codec: the game takes movie pictures as native images (RGBA in the pool's memory)");
    ret_x(c, (uint64_t)((uint32_t)p->slot << 8 | i));
}

// Result ReadDecoderOutputImageRgba(const Pool *, u32 i, u16 x, u16 y, u16 w, u16 h, void *dst, size_t stride, size_t)
static void ov_read_rgba(Cpu *c) {
    CvPool *p = pool_of(arg_x(c, 0));
    uint32_t i = (uint32_t)arg_x(c, 1);
    uint16_t x = (uint16_t)arg_x(c, 2), y = (uint16_t)arg_x(c, 3), w = (uint16_t)arg_x(c, 4), h = (uint16_t)arg_x(c, 5);
    uint8_t *dst = arg_p(c, 6);
    uint64_t stride = arg_x(c, 7), size = arg_x(c, 8);
    const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    if (x || y || stride < (uint64_t)w * 4 || size < stride * (h - 1) + (uint64_t)w * 4) {
        static int once;
        if (!once++)
            rt_log("[RED] codec: ReadDecoderOutputImageRgba region %u,%u %ux%u stride %llu size %llu: not served", x, y,
                   w, h, (unsigned long long)stride, (unsigned long long)size);
        ret_x(c, 0);
        return;
    }
    static int logged;
    if (!logged++) rt_log("codec: the game reads pictures as RGBA (%ux%u, stride %llu)%s", w, h, (unsigned long long)stride,
                          im ? "" : "; this one is not decoded yet");
    if (!im) memset(dst, 0, size);
    else to_rgba(im, dst, w, h, (size_t)stride);
    ret_x(c, 0);
}

// Result ReadDecoderOutputImageNv12(const Pool *, u32 i, u16 x, u16 y, u16 w, u16 h, void *y, size_t ystride,
//                                   size_t ysize, void *uv, size_t uvstride, size_t uvsize)
static void ov_read_nv12(Cpu *c) {
    CvPool *p = pool_of(arg_x(c, 0));
    uint32_t i = (uint32_t)arg_x(c, 1);
    uint16_t x = (uint16_t)arg_x(c, 2), y = (uint16_t)arg_x(c, 3), w = (uint16_t)arg_x(c, 4), h = (uint16_t)arg_x(c, 5);
    uint8_t *ydst = arg_p(c, 6), *uvdst = arg_p(c, 9);
    uint64_t ystride = arg_x(c, 7), ysize = arg_x(c, 8), uvstride = arg_x(c, 10), uvsize = arg_x(c, 11);
    const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    static int logged;
    if (!logged++) rt_log("codec: the game reads pictures as NV12 (%ux%u, strides %llu / %llu)", w, h,
                          (unsigned long long)ystride, (unsigned long long)uvstride);
    if (x || y || ystride != w || uvstride != w || ysize < (uint64_t)w * h || uvsize < (uint64_t)w * h / 2) {
        static int once;
        if (!once++)
            rt_log("[RED] codec: ReadDecoderOutputImageNv12 region %u,%u strides %llu / %llu: only whole tight pictures "
                   "are served", x, y, (unsigned long long)ystride, (unsigned long long)uvstride);
        ret_x(c, 0);
        return;
    }
    if (!im) {
        memset(ydst, 16, ysize);
        memset(uvdst, 128, uvsize);
    } else if (uvdst == ydst + (size_t)w * h) {
        to_nv12(im, ydst, w, h);
    } else {
        uint8_t *tmp = malloc((size_t)w * h * 3 / 2);
        if (!tmp) rt_fatal("codec: NV12 staging allocation");
        to_nv12(im, tmp, w, h);
        memcpy(ydst, tmp, (size_t)w * h);
        memcpy(uvdst, tmp + (size_t)w * h, (size_t)w * h / 2);
        free(tmp);
    }
    ret_x(c, 0);
}

// ---- decoders (VP8 and VP9 share it all; the type holds the host pointer at +0 and the event at +0x100)
static uint32_t dec_init(Cpu *c, uint64_t t, int vp8, uint16_t w, uint16_t h, uint32_t in, uint32_t out) {
    CvDec *d = calloc(1, sizeof *d);
    if (!d) rt_fatal("codec: decoder allocation");
    d->magic = CV_MAGIC;
    d->vp8 = vp8;
    d->w = w;
    d->h = h;
    plat_mutex_init(&d->mu);
    vpx_open(d);
    plat_mutex_lock(&g_mu);
    for (int i = 0; i < CV_DECODERS; i++)
        if (!decoders[i]) {
            decoders[i] = d;
            d->id = (uint32_t)i + 1;
            break;
        }
    plat_mutex_unlock(&g_mu);
    if (!d->id) rt_fatal("codec: more than %d video decoders", CV_DECODERS);
    d->event = t + CV_EVENT_OFF;
    kern_bind_system_event(c, d->event, 0);
    kern_signal_system_event(d->event);
    *(CvDec **)(uintptr_t)t = d;
    rt_log("codec: %s decoder %u: %ux%u, input format %u, output format %u (libvpx on the host)", vp8 ? "VP8" : "VP9",
           d->id, w, h, in, out);
    return 0;
}

static void dec_fin(Cpu *c, uint64_t t) {
    CvDec *d = dec_of(t);
    if (!d) return;
    plat_mutex_lock(&g_mu);   // no sync handle lookup is still inside
    decoders[d->id - 1] = NULL;
    plat_mutex_lock(&d->mu);
    plat_mutex_unlock(&d->mu);
    plat_mutex_unlock(&g_mu);
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->magic = 0;
    // The event goes with the decoder, as in the SDK: a kept one would leave the next decoder made in the same memory
    // with an uninitialised event the game cleared (seen on a game's second movie)
    rt_call(c, sdk_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"), 1, d->event);
    free(d);
    img_spares_free();
    *(CvDec **)(uintptr_t)t = NULL;
}

static uint32_t dec_reg(uint64_t t, uint32_t i, uint64_t pool, uint32_t pidx) {
    CvDec *d = dec_of(t);
    if (!d || i >= CV_BUFS) return CV_BAD;
    plat_mutex_lock(&d->mu);
    CvBuf *b = &d->buf[i];
    *b = (CvBuf){.pool = pool_of(pool), .pidx = pidx, .reg = 1};
    if (d->fcount < CV_FIFO) d->fifo[(d->fhead + d->fcount++) % CV_FIFO] = i;
    else rt_log("[RED] codec: acquirable queue full at register %u", i);
    plat_mutex_unlock(&d->mu);
    return 0;
}

static void drop_index(uint32_t *q, uint32_t *head, uint32_t *count, uint32_t cap, uint32_t i) {
    uint32_t n = 0, tmp[CV_BUFS];
    for (uint32_t k = 0; k < *count; k++) {
        uint32_t v = q[(*head + k) % cap];
        if (v != i) tmp[n++] = v;
    }
    memcpy(q, tmp, n * sizeof *q);
    *head = 0;
    *count = n;
}

static uint32_t dec_unreg(uint64_t t, uint32_t i) {
    CvDec *d = dec_of(t);
    if (!d || i >= CV_BUFS) return CV_BAD;
    plat_mutex_lock(&d->mu);
    d->buf[i].reg = 0;
    d->buf[i].filled = 0;
    d->buf[i].pool = NULL;
    d->buf[i].gen++;
    drop_index(d->fifo, &d->fhead, &d->fcount, CV_FIFO, i);
    drop_index(d->pend, &d->phead, &d->pcount, CV_BUFS, i);
    plat_mutex_unlock(&d->mu);
    return 0;
}

// Stop / Flush: undelivered pictures and a pending end of stream are dropped; the stream restarts at a key frame
static uint32_t dec_reset(uint64_t t) {
    CvDec *d = dec_of(t);
    if (!d) return 0;
    plat_mutex_lock(&d->mu);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->nheld = 0;
    d->eos_pending = 0;
    vpx_open(d);
    kern_signal_system_event(d->event);
    plat_mutex_unlock(&d->mu);
    return 0;
}

// A started decoder with room takes packets whatever became of Initialize's signal: the game's player clears that
// one while not yet started and then counts only later signals (otherwise every movie stays black)
static uint32_t dec_start(uint64_t t) {
    CvDec *d = dec_of(t);
    if (!d) return CV_BAD;
    plat_mutex_lock(&d->mu);
    if (d->nheld < CV_READY) kern_signal_system_event(d->event);
    plat_mutex_unlock(&d->mu);
    return 0;
}

static int hold(CvDec *d, vpx_image_t *im, int64_t ts) {
    if (d->nheld >= CV_HOLD) {
        static int n;
        if (n++ < 4) rt_log("[RED] codec: a packet gave more pictures than the host holds: one dropped");
        vpx_img_free(im);
        return 0;
    }
    d->held[d->nheld] = im;
    d->held_ts[d->nheld] = ts;
    d->held_eos[d->nheld] = 0;
    d->nheld++;
    return 1;
}

// One packet, decoded on the submitting thread (which gives its virtual core away meanwhile: a 1440p VP9 frame takes
// milliseconds). 3603 is the decoder's defined "not now", which the SDK's movie layer retries.
static uint32_t dec_submit(uint64_t *out, uint64_t t, const void *data, uint64_t size, int64_t ts, int eos) {
    CvDec *d = dec_of(t);
    if (!d) return CV_BAD;
    plat_mutex_lock(&d->mu);
    if (d->nheld >= CV_HOLD) {
        kern_clear_system_event(d->event);
        plat_mutex_unlock(&d->mu);
        return CV_BUSY;
    }
    int got = 0;
    if (size && d->vpx_ok) {
        int away = kern_core_away();
        vpx_codec_err_t e = vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0);
        kern_core_back(&away);
        if (e != VPX_CODEC_OK) {
            static int n;
            const char *detail = vpx_codec_error_detail(&d->vpx);
            if (n++ < 4) rt_log("[RED] codec: %s: %s (%s)", d->vp8 ? "VP8" : "VP9", vpx_codec_error(&d->vpx), detail ? detail : "");
        }
        vpx_codec_iter_t it = NULL;
        const vpx_image_t *im;
        while ((im = vpx_codec_get_frame(&d->vpx, &it))) {
            vpx_image_t *copy = img_copy(im);
            if (copy) got += hold(d, copy, ts);
        }
    }
    if (eos) {
        if (got) d->held_eos[d->nheld - 1] = 1;
        else d->eos_pending = 1;
    }
    *out = ++d->submits;
    drain(d, 1);
    plat_mutex_unlock(&d->mu);
    return 0;
}

static uint32_t dec_acquire(uint32_t *idx, CvSync *sync, uint64_t t) {
    CvDec *d = dec_of(t);
    if (!d) return CV_BAD;
    plat_mutex_lock(&d->mu);
    if (!d->fcount) {
        plat_mutex_unlock(&d->mu);
        return CV_NONE;
    }
    uint32_t i = d->fifo[d->fhead];
    d->fhead = (d->fhead + 1) % CV_FIFO;
    d->fcount--;
    *idx = i;
    *sync = (CvSync){CV_SYNC, d->id, i, d->buf[i].gen};
    plat_mutex_unlock(&d->mu);
    return 0;
}

static void dec_release(uint64_t t, uint32_t i) {
    CvDec *d = dec_of(t);
    if (!d || i >= CV_BUFS || !d->buf[i].reg) {
        rt_log("[RED] codec: Release…DecoderOutputBuffer index %u", i);
        return;
    }
    plat_mutex_lock(&d->mu);
    d->buf[i].gen++;
    d->buf[i].filled = 0;
    if (d->pcount < CV_BUFS) d->pend[(d->phead + d->pcount++) % CV_BUFS] = i;
    if (d->fcount < CV_FIFO) d->fifo[(d->fhead + d->fcount++) % CV_FIFO] = i;
    else rt_log("[RED] codec: acquirable queue full at release %u", i);
    drain(d, 0);
    plat_mutex_unlock(&d->mu);
}

static uint32_t dec_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, uint8_t *eos, uint64_t t,
                          uint32_t i) {
    CvDec *d = dec_of(t);
    if (!d || i >= CV_BUFS) return CV_BAD;
    plat_mutex_lock(&d->mu);
    int ok = d->buf[i].filled;
    if (ok) {
        *w = *cw = d->buf[i].w;
        *h = *ch = d->buf[i].h;
        *ts = d->buf[i].ts;
        *eos = d->buf[i].eos != 0;
    }
    plat_mutex_unlock(&d->mu);
    return ok ? 0 : CV_NONE;
}

// ---- the overrides, one set per codec
// Result CalculateVp9DecoderWorkBuffer(size_t *size, size_t *align, size_t bitstream, u16 w, u16 h)
static void ov_work(Cpu *c) {
    *(uint64_t *)arg_p(c, 0) = 0x1000;
    *(uint64_t *)arg_p(c, 1) = 0x1000;
    ret_x(c, 0);
}
// Initialize(Type *, void *work, size_t, size_t bitstream, u16 w, u16 h, u32 refs, InputFormat, OutputFormat)
static void init_work(Cpu *c, int vp8) {
    ret_x(c, dec_init(c, arg_x(c, 0), vp8, (uint16_t)arg_x(c, 4), (uint16_t)arg_x(c, 5), (uint32_t)arg_x(c, 7),
                      (uint32_t)arg_x(c, 8)));
}
// Initialize(Type *, size_t bitstream, u16 w, u16 h, u32 refs, InputFormat, OutputFormat)
static void init_remote(Cpu *c, int vp8) {
    ret_x(c, dec_init(c, arg_x(c, 0), vp8, (uint16_t)arg_x(c, 2), (uint16_t)arg_x(c, 3), (uint32_t)arg_x(c, 5),
                      (uint32_t)arg_x(c, 6)));
}
static void ov_vp9_init(Cpu *c) { init_work(c, 0); }
static void ov_vp9_init2(Cpu *c) { init_remote(c, 0); }
static void ov_vp8_init(Cpu *c) { init_work(c, 1); }
static void ov_vp8_init2(Cpu *c) { init_remote(c, 1); }
static void ov_fin(Cpu *c) { dec_fin(c, arg_x(c, 0)); ret_x(c, 0); }
// Register(Type *, u32 index, VideoDecoderImagePoolType *, u32 pool index)
static void ov_reg(Cpu *c) {
    ctrace("register %u pool index %u", (uint32_t)arg_x(c, 1), (uint32_t)arg_x(c, 3)); ret_x(c, dec_reg(arg_x(c, 0), (uint32_t)arg_x(c, 1), arg_x(c, 2), (uint32_t)arg_x(c, 3))); }
static void ov_unreg(Cpu *c) { ret_x(c, dec_unreg(arg_x(c, 0), (uint32_t)arg_x(c, 1))); }
static void ov_start(Cpu *c) {
    ctrace("start");
    ret_x(c, dec_start(arg_x(c, 0)));
}
static void ov_reset(Cpu *c) {
    ctrace("stop/flush");
    ret_x(c, dec_reset(arg_x(c, 0)));
}
// GetSubmissionReadyEvent(os::SystemEventType **out, Type *)
static void ov_event(Cpu *c) {
    CvDec *d = dec_of(arg_x(c, 1));
    *(uint64_t *)arg_p(c, 0) = d ? d->event : 0;
    ret_x(c, d ? 0 : CV_BAD);
}
// Submit(u64 *out, Type *, const void *, size_t, TimeSpan, bool end_of_stream)
static void ov_submit(Cpu *c) {
    ctrace("submit %llu bytes ts %lld eos %d", (unsigned long long)arg_x(c, 3), (long long)arg_x(c, 4), (int)(arg_x(c, 5) & 1));
    ret_x(c, dec_submit(arg_p(c, 0), arg_x(c, 1), arg_p(c, 2), arg_x(c, 3), (int64_t)arg_x(c, 4), (int)(arg_x(c, 5) & 1)));
}
// Acquire(u32 *index, gs::NativeSyncHandle *, Type *)
static void ov_acquire(Cpu *c) {
    uint32_t r = dec_acquire(arg_p(c, 0), arg_p(c, 1), arg_x(c, 2));
    ctrace("acquire -> %#x index %u", r, r ? 0 : *(uint32_t *)arg_p(c, 0));
    ret_x(c, r);
}
// void Release(Type *, u32 index, gs::NativeSyncHandle) (the handle by value in x2, x3)
static void ov_release(Cpu *c) {
    ctrace("release %u", (uint32_t)arg_x(c, 1));
    dec_release(arg_x(c, 0), (uint32_t)arg_x(c, 1));
}
// GetOutputBufferProperties(u16 *w, u16 *h, u16 *cw, u16 *ch, TimeSpan *, bool *eos, Type *, u32 index)
static void ov_props(Cpu *c) {
    ctrace("properties %u", (uint32_t)arg_x(c, 7));
    ret_x(c, dec_props(arg_p(c, 0), arg_p(c, 1), arg_p(c, 2), arg_p(c, 3), arg_p(c, 4), arg_p(c, 5), arg_x(c, 6),
                       (uint32_t)arg_x(c, 7)));
}
static void ov_not_served(Cpu *c) {
    char lr[96];
    rt_fatal("nn::codec: an H.264 or AV1 decoder from %s: only VP8 and VP9 are served",
             ldr_describe(c->x[30] - 4, lr, sizeof lr));
}

// ---- nn::gs and nn::syncpt: the host's sync handles signal once their buffer holds the picture of that release; a
// handle the host did not make reads as signalled
static int sync_reached(const CvSync *s) {
    if (s->magic != CV_SYNC) {
        static int once;
        if (!once++ && (s->magic || s->dec)) rt_log("[RED] codec: a sync handle the host did not make (%#x): signalled", s->magic);
        return 1;
    }
    if (!s->gen) return 1;
    plat_mutex_lock(&g_mu);
    CvDec *d = s->dec && s->dec <= CV_DECODERS ? decoders[s->dec - 1] : NULL;
    int r = 1;
    if (d && s->idx < CV_BUFS) {
        plat_mutex_lock(&d->mu);
        r = d->buf[s->idx].filled && d->buf[s->idx].filled_gen >= s->gen;
        plat_mutex_unlock(&d->mu);
    }
    plat_mutex_unlock(&g_mu);
    return r;
}
// bool nn::gs::TryWaitNativeSync(const NativeSyncHandle &)
static void ov_trywait(Cpu *c) {
    int r = sync_reached(arg_p(c, 0));
    ctrace("TryWaitNativeSync idx %u gen %u -> %d", ((const CvSync *)arg_p(c, 0))->idx, ((const CvSync *)arg_p(c, 0))->gen, r);
    ret_x(c, (uint64_t)r);
}

// Result CreateSyncPointEvent(SyncPointEventType *): its library is set up by the graphics initialisation the host
// replaces, so the SDK's own jumps through a null table
static void ov_spt_create(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    memset((void *)(uintptr_t)e, 0, 0x58);
    kern_bind_system_event(c, e, 0);
    spt_of(e)->magic = CV_SYNC;
    ret_x(c, 0);
}
// void InitializeMultiWaitHolder(os::MultiWaitHolderType *, SyncPointEventType *): links the event's SystemEvent
static void ov_spt_holder(Cpu *c) {
    rt_call(c, sdk_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE"), 2,
            arg_x(c, 0), arg_x(c, 1));
}
// Result RequestSyncPointEvent(SyncPointEventType *, const SyncPointHandle &): signalled when the buffer is filled
static void ov_spt_request(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    const CvSync *s = arg_p(c, 1);
    ctrace("RequestSyncPointEvent idx %u gen %u magic %#x", s->idx, s->gen, s->magic);
    spt_unarm(e);
    kern_clear_system_event(e);
    spt_of(e)->want = *s;
    spt_of(e)->armed = 1;
    plat_mutex_lock(&spt_mu);
    int k = 0;
    while (k < SPT_MAX && armed[k]) k++;
    if (k < SPT_MAX) armed[k] = e;
    plat_mutex_unlock(&spt_mu);
    if (k == SPT_MAX) {
        rt_log("[RED] codec: more than %d armed sync point events: signalled at once", SPT_MAX);
        kern_signal_system_event(e);
    } else if (sync_reached(s)) {
        spt_unarm(e);
        kern_signal_system_event(e);
    }
    ret_x(c, 0);
}
static void ov_spt_cancel(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    spt_unarm(e);
    kern_clear_system_event(e);
}
static void ov_spt_destroy(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    spt_unarm(e);
    rt_call(c, sdk_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"), 1, e);
    spt_of(e)->magic = 0;
}

HOST_OVERRIDE("_ZN2nn5codec5video10InitializeEv", ov_video_init);
HOST_OVERRIDE("_ZN2nn5codec5video8FinalizeEv", ov_video_fin);
HOST_OVERRIDE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", ov_make_pref);
HOST_OVERRIDE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", ov_pool_size);
HOST_OVERRIDE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE",
              ov_pool_init);
HOST_OVERRIDE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", ov_pool_fin);
HOST_OVERRIDE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", ov_pool_native);
HOST_OVERRIDE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", ov_read_rgba);
HOST_OVERRIDE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", ov_read_nv12);

#define CV_FAMILY(T, init, init2)                                                                                     \
    HOST_OVERRIDE("_ZN2nn5codec29Calculate" #T "DecoderWorkBufferEPmS1_mtt", ov_work);                               \
    HOST_OVERRIDE("_ZN2nn5codec20Initialize" #T "DecoderEPNS0_14" #T                                                   \
                  "DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init);            \
    HOST_OVERRIDE("_ZN2nn5codec20Initialize" #T "DecoderEPNS0_14" #T                                                   \
                  "DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init2);              \
    HOST_OVERRIDE("_ZN2nn5codec18Finalize" #T "DecoderEPNS0_14" #T "DecoderTypeE", ov_fin);                            \
    HOST_OVERRIDE("_ZN2nn5codec24Register" #T "DecoderBufferEPNS0_14" #T                                               \
                  "DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", ov_reg);                                          \
    HOST_OVERRIDE("_ZN2nn5codec26Unregister" #T "DecoderBufferEPNS0_14" #T "DecoderTypeEj", ov_unreg);                 \
    HOST_OVERRIDE("_ZN2nn5codec15Start" #T "DecoderEPNS0_14" #T "DecoderTypeE", ov_start);                             \
    HOST_OVERRIDE("_ZN2nn5codec14Stop" #T "DecoderEPNS0_14" #T "DecoderTypeE", ov_reset);                              \
    HOST_OVERRIDE("_ZN2nn5codec15Flush" #T "DecoderEPNS0_14" #T "DecoderTypeE", ov_reset);                             \
    HOST_OVERRIDE("_ZN2nn5codec33Get" #T "DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14" #T            \
                  "DecoderTypeE", ov_event);                                                                           \
    HOST_OVERRIDE("_ZN2nn5codec27Submit" #T "DecoderInputBufferEPmPNS0_14" #T "DecoderTypeEPKvmNS_8TimeSpanEb",        \
                  ov_submit);                                                                                          \
    HOST_OVERRIDE("_ZN2nn5codec29Acquire" #T "DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14" #T              \
                  "DecoderTypeE", ov_acquire);                                                                         \
    HOST_OVERRIDE("_ZN2nn5codec29Release" #T "DecoderOutputBufferEPNS0_14" #T                                          \
                  "DecoderTypeEjNS_2gs16NativeSyncHandleE", ov_release);                                               \
    HOST_OVERRIDE("_ZN2nn5codec35Get" #T "DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14" #T         \
                  "DecoderTypeEj", ov_props)
CV_FAMILY(Vp9, ov_vp9_init, ov_vp9_init2);
CV_FAMILY(Vp8, ov_vp8_init, ov_vp8_init2);
#undef CV_FAMILY

HOST_OVERRIDE("_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
              ov_not_served);
HOST_OVERRIDE("_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
              ov_not_served);
HOST_OVERRIDE("_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm",
              ov_not_served);
HOST_OVERRIDE("_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm",
              ov_not_served);

HOST_OVERRIDE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", ov_trywait);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", ov_spt_create);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE",
              ov_spt_holder);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE",
              ov_spt_request);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", ov_spt_cancel);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", ov_spt_destroy);
