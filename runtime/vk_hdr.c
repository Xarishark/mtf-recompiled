// HDR output (hdr = 1, experimental): an HDR10 swapchain (Rec. 2020 primaries, PQ) and a final pass that lifts the
// game's SDR picture into it. Settings: hdr (read at start), hdr_peak_nits (the display's peak), hdr_game_nits (the
// picture's white, "paper white") and hdr_ui_nits (the in-game menu's white; the game's HUD is part of the picture and follows hdr_game_nits), the three read when the settings change. With
// hdr = 0 nothing here runs and the present is what it was.
//
// The present chain on an HDR10 swapchain: game -> FSR -> SMAA -> vk_hdr_record -> menu. FSR and SMAA work on an 8-bit
// sRGB image of the swapchain's size (vk_hdr_mid) as on an SDR swapchain. vk_hdr_record then reads it, decodes the sRGB
// curve, lifts the highlights above a knee towards the display's peak (shaders/hdr_encode.frag; all channels scale
// together, so the hue stays), puts the picture's white at hdr_game_nits and writes PQ code values in Rec. 2020 to the
// swapchain image. vk_hdr_ui_*: the in-game menu draws into an RGBA8 image and shaders/hdr_ui.frag blends it onto the
// swapchain at hdr_ui_nits. The game is untouched and its window texture is 8-bit, so the lifted highlights are an
// inverse tonemap of what the game already clipped, not the scene's real range.
#include "vk.h"

#include <math.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "hdr_compose_spv.h"
#include "hdr_encode_spv.h"
#include "hdr_fullscreen_spv.h"
#include "hdr_ui_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_exec.h"
#include "platform/platform.h"

// ---- settings

static int g_wanted = -1;

int vk_hdr_wanted(void) {
    if (g_wanted < 0) g_wanted = cfg_bool("TOTK_HDR");
    return g_wanted;
}

void vk_hdr_off(void) { g_wanted = 0; }

// The three nits, read again when the settings change.
typedef struct { float peak, game, ui; } Nits;
static struct { unsigned seen; int cur; Nits snap[2]; PlatMutex lock; } N = {.seen = ~0u, .lock = PLAT_MUTEX_INIT};

static Nits nits(void) {
    unsigned v = cfg_version();
    if (__atomic_load_n(&N.seen, __ATOMIC_ACQUIRE) != v) {
        plat_mutex_lock(&N.lock);
        if (N.seen != v) {
            Nits *n = &N.snap[!N.cur];
            n->peak = fmaxf((float)cfg_double("TOTK_HDR_PEAK_NITS"), 80.0f);
            n->game = fmaxf((float)cfg_double("TOTK_HDR_GAME_NITS"), 80.0f);
            n->ui = fmaxf((float)cfg_double("TOTK_HDR_UI_NITS"), 80.0f);
            __atomic_store_n(&N.cur, !N.cur, __ATOMIC_RELEASE);
            __atomic_store_n(&N.seen, v, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&N.lock);
    }
    return N.snap[__atomic_load_n(&N.cur, __ATOMIC_ACQUIRE)];
}

// ---- MTF tonemap input tracking (hdr_compose.comp)

int vk_hdr_track;

static int has_aces(const GfxShader *s) {
    uint64_t n = 0;
    const uint32_t *w = gfx_shader_spirv(s, &n);
    static const float want[4] = {0.597190022f, 0.983729005f, 0.432951003f, 1.60475004f};
    uint32_t float_type = 0, found = 0;
    for (uint64_t i = 5; w && i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == 22 && len == 3 && a[1] == 32) float_type = a[0];
        else if (op == 43 && len == 4 && a[0] == float_type && float_type)
            for (int k = 0; k < 4; k++) {
                float f;
                memcpy(&f, &a[2], 4);
                if (f == want[k]) found |= 1u << k;
            }
        i += len;
    }
    return found == 15;
}

static int is_tonemap(const GfxShader *fs) {
    static struct { const GfxShader *s; int yes; } memo[256];
    uintptr_t h = ((uintptr_t)fs >> 4) * 0x9E3779B97F4A7C15ull >> 56;
    for (uint32_t i = 0, j = (uint32_t)h; i < 256; i++, j = (j + 1) & 255) {
        if (memo[j].s == fs) return memo[j].yes;
        if (!memo[j].s) {
            memo[j].yes = has_aces(fs);
            memo[j].s = fs;
            if (memo[j].yes) rt_log("hdr: the game's tonemap found (its ACES constants): the scene goes out in HDR");
            return memo[j].yes;
        }
    }
    return 0;
}

static struct {
    PlatMutex lock;
    VkImage light, bloom;
    VkFormat lfmt, bfmt;
    uint32_t lw, lh;
    float exposure;
    uint64_t frame;
} T = {.lock = PLAT_MUTEX_INIT};

static int exposure_readable(uint64_t a) {
    static uint64_t page, gen;
    uint64_t p = a >> 12, g = vk_mem_gen();
    if (p == page && g == gen && (a & 0xfff) <= 0xffc) return 1;
    if (nvn_gpu_only(a) || !nvn_pool_find(a, 4)) return 0;
    page = p, gen = g;
    return 1;
}

void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info, const GfxShader *fs) {
    if (!fs || !is_tonemap(fs)) return;
    const VkTexImage *tex[2] = {NULL, NULL};
    int ntex = 0;
    float exposure = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || bd->stage != GFX_STAGE_FS) continue;
        if (bd->kind == GFX_RES_TEX && ntex < 2 && bd->res->slot >= 0 && bd->res->slot < 128 && ex->tex[1][bd->res->slot])
            tex[ntex++] = vk_texture_by_id(&ex->tex_pool, NVN_HANDLE_TEX(ex->tex[1][bd->res->slot]), NULL, NULL);
        if (bd->kind == GFX_RES_UBO && bd->res->slot == 5) {
            const p_bind_buf *u = &ex->ubo[1][5 - 3];
            if (u->addr && exposure_readable(u->addr)) memcpy(&exposure, (const void *)(uintptr_t)u->addr, 4);
        }
    }
    if (ntex != 2 || !tex[0] || !tex[1] || !(exposure > 0) || !isfinite(exposure)) return;
    if ((uint64_t)tex[1]->width * tex[1]->height > (uint64_t)tex[0]->width * tex[0]->height) {
        const VkTexImage *t = tex[0];
        tex[0] = tex[1], tex[1] = t;
    }
    plat_mutex_lock(&T.lock);
    T.light = tex[0]->image, T.lfmt = tex[0]->format, T.lw = tex[0]->width, T.lh = tex[0]->height;
    T.bloom = tex[1]->image, T.bfmt = tex[1]->format;
    T.exposure = exposure;
    T.frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    plat_mutex_unlock(&T.lock);
}

void vk_hdr_image_gone(VkImage image) {
    if (!vk_hdr_track) return;
    plat_mutex_lock(&T.lock);
    if (T.light == image) T.light = VK_NULL_HANDLE;
    if (T.bloom == image) T.bloom = VK_NULL_HANDLE;
    plat_mutex_unlock(&T.lock);
}

// ---- swapchain

// With hdr = 1, the HDR10 format among the surface's (10-bit UNORM, ST 2084): 1 with *out set, else 0 and the
// reason logged once.
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    if (!vk_hdr_wanted() || !vk_ctx()->has_swapchain_colorspace) return 0;
    static int logged;
    if (!logged) {
        char b[1024];
        int o = 0;
        for (uint32_t i = 0; i < n && o < (int)sizeof b - 32; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %d/%d", (int)f[i].format, (int)f[i].colorSpace);
        rt_log("hdr: surface formats (VkFormat/VkColorSpaceKHR):%s", b);
    }
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                f[i].format == (pass ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
                *out = f[i];
                if (!logged) rt_log("hdr: HDR10 swapchain (format %d)", (int)f[i].format);
                logged = 1;
                return 1;
            }
    if (!logged)
        rt_log("hdr: the display offers no HDR10 format, so the picture stays SDR (Windows: turn on \"Use HDR\" in the "
               "display settings; Linux: a Wayland compositor with colour management and HDR on)");
    logged = 1;
    return 0;
}

// The content's peak and the picture's white for sc, sent again when either changes (vk_present.c calls it on every
// present to an HDR10 swapchain, with new = 1 for a new swapchain). The lifted SDR picture reaches hdr_peak_nits. With
// native HDR the game grades to its own peak, the display's capped by the game's limit for the mode (ov_gfx.c
// hdr_game_content_peak: 1400 nits in 3.2.0 gameplay with any higher hdr_peak_nits); a display told hdr_peak_nits
// would tone-map for a range the picture never reaches.
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) {
    static unsigned sent = ~0u;
    static int sent_game_peak = -1;
    VkCtx *c = vk_ctx();
    int game_peak = hdr_native_on() ? hdr_game_content_peak() : 0;
    if (!c->has_hdr_metadata || (!new_sc && cfg_version() == sent && game_peak == sent_game_peak)) return;
    sent = cfg_version();
    sent_game_peak = game_peak;
    PFN_vkSetHdrMetadataEXT set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(c->device, "vkSetHdrMetadataEXT");
    if (!set) return;
    Nits n = nits();
    float peak = game_peak > 0 && game_peak < n.peak ? (float)game_peak : n.peak;
    VkHdrMetadataEXT m = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                          .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                          .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                          .maxLuminance = peak, .minLuminance = 0.0f, .maxContentLightLevel = peak,
                          .maxFrameAverageLightLevel = n.game};
    set(c->device, 1, &sc, &m);
    static float logged = -1;
    if (peak != logged) rt_log("hdr: metadata to the display: peak %.0f nits, paper white %.0f%s", peak, n.game,
                               game_peak > 0 && peak == (float)game_peak ? " (the game's own peak)" : "");
    logged = peak;
}

// ---- 3. present passes

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

typedef struct { VkImage img; VmaAllocation alloc; VkImageView view; uint32_t w, h; } Img;

static void img_free(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    *m = (Img){0};
}

// m at w x h in fmt; another size waits for the device first (earlier presents may still read the old image).
static int img_ensure(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (m->img && m->w == w && m->h == h) return 1;
    if (m->img) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        img_free(c, m);
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) {
        m->view = VK_NULL_HANDLE;
        img_free(c, m);
        return 0;
    }
    m->w = w, m->h = h;
    return 1;
}

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler sampler, linear;
    VkDescriptorSetLayout dsl, dsl_comp;
    VkPipelineLayout lay, comp_layout;
    VkPipeline hdr, ui, comp;
    VkFormat pipe_fmt;
    Img mid;                        // the chain's 8-bit sRGB target, swapchain-sized
    Img menu;                       // the in-game menu, RGBA8
    Img out;                        // PQ code values from hdr_compose.comp, RGBA16F
    VkImageView views[2];           // light, bloom
    float vk, yk, slope;
} H;

static VkShaderModule module(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static double aces_fit(double v) { return (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081); }

static void knee(float yk) {
    double a = 1.0 - yk * 0.983729, b = 0.0245786 - yk * 0.4329510, cc = -(0.000090537 + yk * 0.238081);
    double v = (-b + sqrt(b * b - 4.0 * a * cc)) / (2.0 * a), e = 1e-4 * v;
    H.vk = (float)v, H.yk = yk;
    H.slope = (float)((aces_fit(v + e) - aces_fit(v - e)) / (2.0 * e));
}

static int init(VkCtx *c) {
    H.tried = 1;
    H.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 1, .pBindings = &b};
    const VkShaderStageFlags CS = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding comp[4] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = CS},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = CS},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = CS},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = CS}};
    VkDescriptorSetLayoutCreateInfo cci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = comp};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkSamplerCreateInfo li = si;
    li.magFilter = li.minFilter = VK_FILTER_LINEAR;
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPushConstantRange cpr = {CS, 0, 48};
    if (!H.push || vkCreateSampler(c->device, &si, NULL, &H.sampler) != VK_SUCCESS ||
        vkCreateSampler(c->device, &li, NULL, &H.linear) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(c->device, &dci, NULL, &H.dsl) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(c->device, &cci, NULL, &H.dsl_comp) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li_pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                        .pSetLayouts = &H.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayoutCreateInfo cli_pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &H.dsl_comp, .pushConstantRangeCount = 1, .pPushConstantRanges = &cpr};
    if (vkCreatePipelineLayout(c->device, &li_pi, NULL, &H.lay) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &cli_pi, NULL, &H.comp_layout) != VK_SUCCESS) goto fail;
    VkShaderModule cm = module(c, hdr_compose_spv, sizeof hdr_compose_spv);
    if (cm) {
        VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                           .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage = CS, .module = cm, .pName = "main"},
                                           .layout = H.comp_layout};
        if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &cpi, NULL, &H.comp) != VK_SUCCESS) H.comp = VK_NULL_HANDLE;
        vkDestroyShaderModule(c->device, cm, NULL);
    }
    knee(0.8f);
    return H.ok = 1;
fail:
    rt_log("hdr: the present passes could not be created; falling back to SDR");
    return 0;
}

// The image the chain (blit or FSR, then SMAA) writes in place of the swapchain image: RGBA8 UNORM of the swapchain's
// size in TRANSFER_DST_OPTIMAL, its old content dropped. VK_NULL_HANDLE: no HDR pass, the caller presents SDR.
VkImage vk_hdr_mid(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.tried) init(c);
    if (vk_hdr_wanted()) vk_hdr_track = 1;
    if (!H.ok || !img_ensure(c, &H.mid, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_NULL_HANDLE;
    barrier(cb, H.mid.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return H.mid.img;
}

// The in-game menu's target: an RGBA8 image of the swapchain's size in COLOR_ATTACHMENT_OPTIMAL (its old content
// dropped); the caller renders the menu into the view with a clear to 0.
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.menu, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_NULL_HANDLE;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return H.menu.view;
}

// A full-screen triangle pipeline into a colour attachment of format fmt; blend: the fragment shader's alpha over dst.
static VkPipeline frag_pipeline(VkCtx *c, VkFormat fmt, const uint32_t *code, size_t size, int blend) {
    VkShaderModule vs = module(c, hdr_fullscreen_spv, sizeof hdr_fullscreen_spv), fs = module(c, code, size);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = blend ? VK_TRUE : VK_FALSE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = H.lay};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &p) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    return r == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

static int pipelines(VkCtx *c, VkFormat fmt) {
    if (H.hdr && H.ui && H.pipe_fmt == fmt) return 1;
    if (H.hdr || H.ui) {   // another swapchain format: earlier presents may still use the old pipelines
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        if (H.hdr) vkDestroyPipeline(c->device, H.hdr, NULL);
        if (H.ui) vkDestroyPipeline(c->device, H.ui, NULL);
        H.hdr = H.ui = VK_NULL_HANDLE;
    }
    H.hdr = frag_pipeline(c, fmt, hdr_encode_spv, sizeof hdr_encode_spv, 0);
    H.ui = frag_pipeline(c, fmt, hdr_ui_spv, sizeof hdr_ui_spv, 1);
    if (!H.hdr || !H.ui) {
        rt_log("hdr: the present pipelines could not be created; falling back to SDR");
        return 0;
    }
    H.pipe_fmt = fmt;
    return 1;
}

// One full-screen draw of src_view (SHADER_READ_ONLY_OPTIMAL) into dst_view (COLOR_ATTACHMENT_OPTIMAL), w x h.
static void fullscreen(VkCommandBuffer cb, VkPipeline pipe, VkImageView dst_view, int load, VkImageView src_view,
                       const float pc[4], uint32_t w, uint32_t h) {
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkDescriptorImageInfo ii = {H.sampler, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.lay, 0, 1, &wr);
    vkCmdPushConstants(cb, H.lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

static VkImageView view_of(VkCtx *c, VkImageView *keep, VkImage img, VkFormat fmt) {
    if (*keep) vkDestroyImageView(c->device, *keep, NULL);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, keep) != VK_SUCCESS) *keep = VK_NULL_HANDLE;
    return *keep;
}

// The HDR pass: the chain's image (vk_hdr_mid, TRANSFER_DST_OPTIMAL after the blit or FSR and SMAA) onto dst (the
// swapchain image, UNDEFINED), left in COLOR_ATTACHMENT_OPTIMAL. 0: no pass, dst untouched and the caller presents SDR.
int vk_hdr_record(VkCommandBuffer cb, VkImage dst, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.ok || !H.mid.img || !pipelines(c, fmt)) return 0;
    plat_mutex_lock(&T.lock);
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int scene = H.comp && T.light && T.bloom && now - T.frame <= 2;
    VkImage light = T.light, bloom = T.bloom;
    VkFormat lfmt = T.lfmt, bfmt = T.bfmt;
    float exposure = T.exposure;
    plat_mutex_unlock(&T.lock);
    if (scene && img_ensure(c, &H.out, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        view_of(c, &H.views[0], light, lfmt) && view_of(c, &H.views[1], bloom, bfmt)) {
        Nits n = nits();
        const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier(cb, H.mid.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cb, light, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cb, bloom, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cb, H.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, ALL, 0, CS,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        VkDescriptorImageInfo ii[3] = {{H.sampler, H.mid.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {H.sampler, H.views[0], VK_IMAGE_LAYOUT_GENERAL},
                                       {H.linear, H.views[1], VK_IMAGE_LAYOUT_GENERAL}};
        VkDescriptorImageInfo oi = {VK_NULL_HANDLE, H.out.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[4];
        for (int i = 0; i < 4; i++)
            wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                           .descriptorCount = 1,
                                           .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           .pImageInfo = i < 3 ? &ii[i] : &oi};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.comp);
        H.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.comp_layout, 0, 4, wr);
        struct { int32_t offset[2], size[2]; float n[4], k[4]; } pc = {
            {0, 0}, {(int32_t)w, (int32_t)h}, {n.ui, n.game, n.peak, exposure},
            {H.vk, H.yk, H.slope, fmaxf(n.peak / n.game, H.yk + 0.01f)}};
        vkCmdPushConstants(cb, H.comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        barrier(cb, H.out.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CS,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
        vkCmdBlitImage(cb, H.out.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &r, VK_FILTER_NEAREST);
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        return 1;
    }
    barrier(cb, H.mid.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    Nits n = nits();
    const float pc[4] = {n.game, n.peak, 0.8f, 0};   // the picture's white and the display's peak in nits; the knee
    fullscreen(cb, H.hdr, dst_view, 0, H.mid.view, pc, w, h);
    return 1;
}

// The menu image onto dst (a swapchain image in COLOR_ATTACHMENT_OPTIMAL, view dst_view of format fmt).
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.ok || !H.menu.img || !pipelines(c, fmt)) return;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const float pc[4] = {nits().ui, 0, 0, 0};
    fullscreen(cb, H.ui, dst_view, 1, H.menu.view, pc, w, h);
}

void vk_hdr_release(void) {
    VkCtx *c = vk_ctx();
    img_free(c, &H.mid);
    img_free(c, &H.menu);
    img_free(c, &H.out);
    for (int i = 0; i < 2; i++)
        if (H.views[i]) vkDestroyImageView(c->device, H.views[i], NULL);
    if (H.hdr) vkDestroyPipeline(c->device, H.hdr, NULL);
    if (H.ui) vkDestroyPipeline(c->device, H.ui, NULL);
    if (H.comp) vkDestroyPipeline(c->device, H.comp, NULL);
    if (H.lay) vkDestroyPipelineLayout(c->device, H.lay, NULL);
    if (H.comp_layout) vkDestroyPipelineLayout(c->device, H.comp_layout, NULL);
    if (H.dsl) vkDestroyDescriptorSetLayout(c->device, H.dsl, NULL);
    if (H.dsl_comp) vkDestroyDescriptorSetLayout(c->device, H.dsl_comp, NULL);
    if (H.sampler) vkDestroySampler(c->device, H.sampler, NULL);
    if (H.linear) vkDestroySampler(c->device, H.linear, NULL);
    memset(&H, 0, sizeof H);
}

// PQ code (0..1023) of a 10-bit channel to the sRGB byte of the same colour at the HUD's white (menu test shots and
// flight thumbnails of an HDR10 swapchain).
static float pq_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(e, 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) {
    float ui = nits().ui;
    // Rec. 2020 to Rec. 709 (the inverse of ITU-R BT.2087's matrix)
    static const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
    for (size_t i = 0; i < px; i++) {
        uint32_t v;
        memcpy(&v, in + 4 * i, 4);
        float c[3] = {(float)(v & 1023), (float)(v >> 10 & 1023), (float)(v >> 20 & 1023)};   // A2B10G10R10: R low
        if (a2r) { float t = c[0]; c[0] = c[2]; c[2] = t; }
        for (int k = 0; k < 3; k++) c[k] = pq_nits(c[k] / 1023.0f) / ui;
        for (int k = 0; k < 3; k++) {
            float l = fminf(fmaxf(m[3 * k] * c[0] + m[3 * k + 1] * c[1] + m[3 * k + 2] * c[2], 0.0f), 1.0f);
            float s = l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
            out[4 * i + k] = (uint8_t)(s * 255.0f + 0.5f);
        }
        out[4 * i + 3] = 255;
    }
}
