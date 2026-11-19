// NVN2 (Switch 2) procs beyond the NVN1 core: program builders, primary queues, ray tracing
// memory queries, indexed fast clears, and the window's colour space and HDR metadata.
//
// The window's colour space and HDR metadata are kept and logged once: they tell whether MTF asks for
// an HDR output (the present path is SDR until that is known).
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

void nvnp_nvnProgramInitialize(Cpu *c);
void nvnp_nvnProgramSetShaders(Cpu *c);

// ---- program builders: SetDefaults, SetDevice, SetShaders(count, NVNshaderData[]), then InitializeFromBuilder, which
// is nvnProgramInitialize + nvnProgramSetShaders. The builder keeps the caller's array, as other NVN builders do.
typedef struct { uint64_t device, shaders; int32_t count, sample_shading; uint64_t hashes; } ProgBuilder;

NVN_API(nvnProgramBuilderSetDefaults) { memset(arg_p(c, 0), 0, sizeof(ProgBuilder)); }
NVN_API(nvnProgramBuilderSetDevice) { ((ProgBuilder *)arg_p(c, 0))->device = arg_x(c, 1); }
NVN_API(nvnProgramBuilderGetDevice) { ret_x(c, ((const ProgBuilder *)arg_p(c, 0))->device); }
NVN_API(nvnProgramBuilderSetShaders) {
    ProgBuilder *b = arg_p(c, 0);
    b->count = (int32_t)arg_x(c, 1);
    b->shaders = arg_x(c, 2);
}
NVN_API(nvnProgramBuilderGetShaders) { ret_x(c, ((const ProgBuilder *)arg_p(c, 0))->shaders); }
NVN_API(nvnProgramBuilderGetShaderCount) { ret_x(c, (uint64_t)(uint32_t)((const ProgBuilder *)arg_p(c, 0))->count); }
NVN_API(nvnProgramBuilderSetSampleShading) { ((ProgBuilder *)arg_p(c, 0))->sample_shading = (int32_t)arg_x(c, 1); }
NVN_API(nvnProgramBuilderGetSampleShading) { ret_x(c, (uint64_t)(uint32_t)((const ProgBuilder *)arg_p(c, 0))->sample_shading); }
NVN_API(nvnProgramBuilderSetDebugDataHashes) { ((ProgBuilder *)arg_p(c, 0))->hashes = arg_x(c, 1); }
NVN_API(nvnProgramBuilderGetDebugDataHashes) { ret_x(c, ((const ProgBuilder *)arg_p(c, 0))->hashes); }

// NVNboolean nvnProgramInitializeFromBuilder(NVNprogram *, const NVNprogramBuilder *)
NVN_API(nvnProgramInitializeFromBuilder) {
    uint64_t program = arg_x(c, 0);
    const ProgBuilder *b = arg_p(c, 1);
    uint64_t saved[3] = {c->x[0], c->x[1], c->x[2]};
    c->x[0] = program, c->x[1] = b->device;
    nvnp_nvnProgramInitialize(c);
    uint64_t ok = c->x[0] & 0xff;
    if (ok && b->count) {
        c->x[0] = program, c->x[1] = (uint64_t)(uint32_t)b->count, c->x[2] = b->shaders;
        nvnp_nvnProgramSetShaders(c);
        ok = c->x[0] & 0xff;
    }
    c->x[1] = saved[1], c->x[2] = saved[2];
    (void)saved[0];
    ret_x(c, ok);
}

// ---- queues: every queue's submits run in order on the one back end, so a primary queue needs no work; no ray
// tracing (the game reserves none it uses)
static uint64_t primary_queue, ray_tracing_size;
NVN_API(nvnQueueBuilderSetPrimaryQueue) { primary_queue = arg_x(c, 1); }
NVN_API(nvnQueueBuilderGetPrimaryQueue) { ret_x(c, primary_queue); }
NVN_API(nvnQueueBuilderSetRayTracingMemorySize) { ray_tracing_size = arg_x(c, 1); }
NVN_API(nvnQueueBuilderGetRayTracingMemorySize) { ret_x(c, ray_tracing_size); }
NVN_API(nvnQueueGetTotalRayTracingMemoryUsed) { ret_x(c, 0); }

// ---- device
NVN_API(nvnDeviceGetFlags) { ret_x(c, 0); }
// Indexed fast-clear registrations: like the unindexed ones (nvn_api_core.c), not registered; a clear is a clear
NVN_API(nvnDeviceRegisterFastClearColorIndex) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColoriIndex) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColoruiIndex) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearDepthIndex) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearStencil) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearStencilIndex) { ret_x(c, 0); }

// ---- window colour space and HDR metadata (kept per process: the game has one window)
#define HDR_METADATA_BYTES 0x40
static int32_t color_space;
static uint8_t hdr_content[HDR_METADATA_BYTES], hdr_display[HDR_METADATA_BYTES];
static PlatMutex hdr_lock = PLAT_MUTEX_INIT;

static void note(const char *what, const void *data, size_t n) {
    static int logged[4];
    int k = what[0] == 'c' ? 0 : what[8] == 'c' ? 1 : 2;
    if (logged[k]++) return;
    char hex[3 * 16 + 1] = "";
    for (size_t i = 0; i < n && i < 16; i++) snprintf(hex + 3 * i, 4, "%02x ", ((const uint8_t *)data)[i]);
    rt_log("nvn2: the game sets the window's %s: %s(the present stays SDR)", what, hex);
}

NVN_API(nvnWindowBuilderSetColorSpace) {
    color_space = (int32_t)arg_x(c, 1);
    note("colour space", &color_space, 4);
}
NVN_API(nvnWindowBuilderGetColorSpace) { ret_x(c, (uint64_t)(uint32_t)color_space); }

static void set_meta(uint8_t *dst, const void *src, const char *what) {
    if (!src) return;
    plat_mutex_lock(&hdr_lock);
    memcpy(dst, src, HDR_METADATA_BYTES);
    plat_mutex_unlock(&hdr_lock);
    note(what, src, HDR_METADATA_BYTES);
}
static void get_meta(void *dst, const uint8_t *src) {
    if (!dst) return;
    plat_mutex_lock(&hdr_lock);
    memcpy(dst, src, HDR_METADATA_BYTES);
    plat_mutex_unlock(&hdr_lock);
}
NVN_API(nvnWindowBuilderSetHdrContentMetadata) { set_meta(hdr_content, arg_p(c, 1), "HDR content metadata"); }
NVN_API(nvnWindowBuilderGetHdrContentMetadata) { get_meta(arg_p(c, 1), hdr_content); }
NVN_API(nvnWindowBuilderSetHdrDisplayMetadata) { set_meta(hdr_display, arg_p(c, 1), "HDR display metadata"); }
NVN_API(nvnWindowBuilderGetHdrDisplayMetadata) { get_meta(arg_p(c, 1), hdr_display); }
NVN_API(nvnWindowSetHdrContentMetadata) { set_meta(hdr_content, arg_p(c, 1), "HDR content metadata"); }
NVN_API(nvnWindowGetHdrContentMetadata) { get_meta(arg_p(c, 1), hdr_content); }
NVN_API(nvnWindowSetHdrDisplayMetadata) { set_meta(hdr_display, arg_p(c, 1), "HDR display metadata"); }
NVN_API(nvnWindowGetHdrDisplayMetadata) { get_meta(arg_p(c, 1), hdr_display); }

// ---- NVN2 procs that need no work here (each says why)
NVN_API(nvnCommandBufferBindShadingRateImage) { (void)c; }
NVN_API(nvnCommandBufferSetComputeOverlapMode) { (void)c; }   // nvnCommandBufferSetComputeOverlapMode %d: the host runs compute in submission order
NVN_API(nvnCommandBufferSetComputeWorkloadPriority) { (void)c; }   // nvnCommandBufferSetComputeWorkloadPriority %d %d: the host runs compute in submission order, with no priority
NVN_API(nvnCommandBufferSetGeometryProcessingMode) { (void)c; }   // nvnCommandBufferSetGeometryProcessingMode %d: no effect on the host
NVN_API(nvnCommandBufferSetRecordingMode) { (void)c; }   // nvnCommandBufferSetRecordingMode %d: the host records every command buffer alike
NVN_API(nvnCommandBufferSetShadingRatePaletteMode) { (void)c; }
NVN_API(nvnCommandBufferShadingRateImagePalette) { (void)c; }
NVN_API(nvnDlssNetworkBuilderGetStorageSize) { ret_x(c, (uint64_t)(0x1000)); }
NVN_API(nvnDlssNetworkBuilderSetDevice) { (void)c; }
NVN_API(nvnDlssNetworkBuilderSetMaxEvaluationEntries) { (void)c; }
NVN_API(nvnDlssNetworkBuilderSetStorage) { (void)c; }
NVN_API(nvnDlssNetworkBuilderSetTextureIDs) { (void)c; }
NVN_API(nvnDlssNetworkFinalize) { (void)c; }
NVN_API(nvnDlssParametersSetEvaluationIndex) { (void)c; }
NVN_API(nvnSetExternalDebuggerEnabledNVX) { (void)c; }

// ---- Features MTF's renderer has calls to (tools/nvn_proc_usage.py) that no scene played so far reached: ray tracing,
// mesh shaders, transform feedback, fixed tessellation levels, two DLSS entry points. Each one does nothing and says so
// in red the first time, instead of ending the game, so a scene that needs one shows up in the log.
#define NOT_HERE(name, what)                                                                                         \
    NVN_API(name) {                                                                                                  \
        (void)c;                                                                                                     \
        static volatile int32_t once;                                                                                \
        if (!plat_xchg32(&once, 1)) rt_log("[RED] nvn: " #name " called: " what);                                    \
    }
NOT_HERE(nvnCommandBufferBindAccelerationStructure, "ray tracing is not implemented")
NOT_HERE(nvnCommandBufferBindAccelerationStructures, "ray tracing is not implemented")
NOT_HERE(nvnCommandBufferBuildAccelerationStructures, "ray tracing is not implemented")
NOT_HERE(nvnCommandBufferCompactAccelerationStructure, "ray tracing is not implemented")
NOT_HERE(nvnCommandBufferCopyAccelerationStructure, "ray tracing is not implemented")
NOT_HERE(nvnCommandBufferRelocateAccelerationStructure, "ray tracing is not implemented")
NOT_HERE(nvnCommandBufferDispatchRays, "ray tracing is not implemented: nothing drawn")
NOT_HERE(nvnCommandBufferDispatchRaysIndirect, "ray tracing is not implemented: nothing drawn")
NOT_HERE(nvnDeviceCalculateAccelerationStructureMemoryRequirements, "ray tracing is not implemented: no sizes written")
NOT_HERE(nvnCommandBufferDrawMeshTasks3D, "mesh shaders are not implemented: nothing drawn")
NOT_HERE(nvnCommandBufferDrawMeshTasks3DIndirect, "mesh shaders are not implemented: nothing drawn")
NOT_HERE(nvnCommandBufferMultiDrawMeshTasks3DIndirect, "mesh shaders are not implemented: nothing drawn")
NOT_HERE(nvnCommandBufferMultiDrawMeshTasks3DIndirectCount, "mesh shaders are not implemented: nothing drawn")
NOT_HERE(nvnCommandBufferBindTransformFeedbackBuffer, "transform feedback is not implemented")
NOT_HERE(nvnCommandBufferBindTransformFeedbackBuffers, "transform feedback is not implemented")
NOT_HERE(nvnCommandBufferEndTransformFeedback, "transform feedback is not implemented")
NOT_HERE(nvnCommandBufferSetOuterTessellationLevels, "fixed tessellation levels (no control shader) are not implemented")
NOT_HERE(nvnCommandBufferSetInnerTessellationLevels, "fixed tessellation levels (no control shader) are not implemented")
NOT_HERE(nvnDlssParametersSetMotionVectorScale, "the game's own DLSS is not run (DLAA is the host's)")
NOT_HERE(nvnCommandBufferEvaluateDlss, "the game's own DLSS is off: nothing evaluated (the host's DLSS, vk_dlaa.c, runs instead)")
// NVNboolean nvnDlssNetworkInitialize(...): reports failure, so the game's own DLSS stays off and the game keeps its
// own path (its FXAA and scaling draw, which the host's DLSS replaces: vk_dlaa.c)
NVN_API(nvnDlssNetworkInitialize) {
    static volatile int32_t once;
    if (!plat_xchg32(&once, 1)) rt_log("[RED] nvn: nvnDlssNetworkInitialize called: answered as failed");
    ret_x(c, 0);
}

// NVNboolean nvnQueueWaitSync(NVNqueue *, const NVNsync *): a GPU-side wait for another queue's fence. Every queue's
// submits replay in order on the one back end, so a fence already recorded is behind every later submit.
NVN_API(nvnQueueWaitSync) { ret_x(c, 1); }

// void nvnCommandBufferSetViewportSwizzles(NVNcommandBuffer *, int first, int count, const NVNviewportSwizzle *): per
// viewport the source of clip x, y, z, w (four ints each). Identity changes nothing. Others are not applied yet (open:
// swapping the clip position in the vertex stage, or VK_NV_viewport_swizzle, would apply them): logged once, the draws render unswizzled.
NVN_API(nvnCommandBufferSetViewportSwizzles) {
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const int32_t *s = arg_p(c, 3);
    for (int v = 0; s && v < n && v < 16; v++)
        for (int k = 0; k < 4; k++)
            if (s[4 * v + k] != 2 * k) {
                static volatile int32_t logged;
                if (!plat_xchg32(&logged, 1))
                    rt_log("[RED] nvn2: viewport %d swizzle %d %d %d %d is not applied (first %d, %d viewports)",
                           first + v, s[4 * v], s[4 * v + 1], s[4 * v + 2], s[4 * v + 3], first, n);
                return;
            }
}
