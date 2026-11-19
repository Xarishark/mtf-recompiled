// Graphics bring-up:
//   nv::*        record the game's allocator callbacks and graphics work memory (guest addresses) for the NVN layer
//   nn::vi::*    display, layer and native window identities; the vsync event is bound here
//   nvnBootstrapLoader   the end of --boot (rt_boot_event "nvn"); otherwise the synthetic address of an NVN proc (nvn_procs.c)
// Contains adapted code under the MIT licence: authors and licence texts in THIRD_PARTY_NOTICES.md.
#include <stdint.h>

#include "gfx.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

#define VI_R_INVALID 0xf601u   // the result for a call on an unknown display or layer

static OvNvState nv;
static PlatMutex lock = PLAT_MUTEX_INIT;

static void set_allocator(Cpu *c, OvNvAllocator *a) {
    plat_mutex_lock(&lock);
    *a = (OvNvAllocator){arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
    plat_mutex_unlock(&lock);
}

// void nv::SetGraphicsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_graphics_allocator(Cpu *c) { set_allocator(c, &nv.graphics); }
// void nv::SetGraphicsDevtoolsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_devtools_allocator(Cpu *c) { set_allocator(c, &nv.devtools); }

// void nv::InitializeGraphics(void *memory, size_t size)
static void ov_initialize_graphics(Cpu *c) {
    uint64_t mem = arg_x(c, 0), size = arg_x(c, 1);
    // A Switch 2 game passes no work memory (MTF: 0, 0): its graphics driver takes the remote memory the
    // graphics system boots with (nn::gs), which this host stands in for. Nothing here allocates from it either way.
    if (!mem != !size) rt_fatal("nv::InitializeGraphics: graphics work memory %#llx with size %#llx",
                                (unsigned long long)mem, (unsigned long long)size);
    plat_mutex_lock(&lock);
    nv.memory = mem;
    nv.memory_size = size;
    nv.initialized = 1;
    plat_mutex_unlock(&lock);
    rt_log("nv: graphics work memory %#llx+%#llx recorded", (unsigned long long)mem,
           (unsigned long long)size);
}

// void nv::InitCheck(): aborts unless graphics are initialised. The SDK's own version tests the flag its
// nv::InitializeGraphics sets, which the host replaces (above); the host's record answers instead.
// MTF's movie::VideoDecoder::Initialize calls it.
static void ov_init_check(Cpu *c) {
    char lr[96];
    if (!nv.initialized)
        rt_fatal("nv::InitCheck from %s: graphics are not initialised (no nv::InitializeGraphics yet)",
                 ldr_describe(c->x[30] - 4, lr, sizeof lr));
}
HOST_OVERRIDE("_ZN2nv9InitCheckEv", ov_init_check);

// ---- nn::vi: the identities are small zeroed blocks in the guest-visible kernel region, in case SDK code reads them
static int vi_ready;
static void *display, *layer, *window;

// void nn::vi::Initialize()
static void ov_vi_initialize(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    if (!display) {
        display = mem_kernel_alloc(64, 16);
        layer = mem_kernel_alloc(64, 16);
        window = mem_kernel_alloc(64, 16);
    }
    vi_ready = 1;
    plat_mutex_unlock(&lock);
}

static int vi_up(void) {
    plat_mutex_lock_shared(&lock);
    int r = vi_ready;
    plat_mutex_unlock_shared(&lock);
    return r;
}

// Result nn::vi::OpenDefaultDisplay(Display **out)
static void ov_open_display(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up()) { ret_x(c, VI_R_INVALID); return; }
    *out = display;
    ret_x(c, 0);
}

// Result nn::vi::CreateLayer(Layer **out, Display *display)
void ov_resolution_layer(Cpu *c);   // ov_resolution.c
static void ov_create_layer(Cpu *c) {
    ov_resolution_layer(c);
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    *out = layer;
    ret_x(c, 0);
}

// Result nn::vi::GetNativeWindow(void **out, Layer *layer): a placeholder the NVN layer recognises
static void ov_native_window(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != layer) { ret_x(c, VI_R_INVALID); return; }
    *out = window;
    ret_x(c, 0);
}

// Result nn::vi::GetDisplayVsyncEvent(os::SystemEventType *event, Display *display): auto-clear system event
static void ov_vsync_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    kern_bind_system_event(c, ev, 1);
    gfx_vsync_bind(ev);
    ret_x(c, 0);
}

// void *nvnBootstrapLoader(const char *name): the first NVN call
static void ov_nvn_bootstrap(Cpu *c) {
    const char *name = arg_p(c, 0);
    if (!name) name = "(null)";
    static int32_t first = 1;
    if (rt_config()->boot && plat_xchg32(&first, 0)) rt_boot_event("nvn", c, name);   // ends --boot; returns
                                                                                                // when --seconds goes on
    ret_x(c, nvn_proc_address(name));
}

// The NVN procs need their synthetic addresses before hostfn_seal; the loader seals at the end of rt_boot, so the
// registration runs at start-up instead of through boot.c.
__attribute__((constructor)) static void ov_nvn_procs_register(void) { nvn_procs_register(); }

HOST_OVERRIDE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_graphics_allocator);
HOST_OVERRIDE("_ZN2nv18InitializeGraphicsEPvm", ov_initialize_graphics);
HOST_OVERRIDE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_devtools_allocator);
HOST_OVERRIDE("_ZN2nn2vi10InitializeEv", ov_vi_initialize);
HOST_OVERRIDE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", ov_open_display);
HOST_OVERRIDE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", ov_create_layer);
HOST_OVERRIDE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", ov_native_window);
HOST_OVERRIDE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", ov_vsync_event);
HOST_OVERRIDE("nvnBootstrapLoader", ov_nvn_bootstrap);
