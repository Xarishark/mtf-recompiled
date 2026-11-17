// Lightweight top-right FPS overlay rendered via staging buffer -> VkImage -> vkCmdBlitImage.
// Zero pipelines or shaders required; controlled by PERF_OVERLAY (off | fps | full).
#include "vk.h"
#include "cfg.h"
#include "platform/platform.h"

#include <stdio.h>
#include <string.h>

#define FPS_W 128
#define FPS_H 28

static struct {
    int init;
    int failed;
    VkBuffer buf;
    VkDeviceMemory buf_mem;
    void *mapped;
    VkImage img;
    VkDeviceMemory img_mem;
    VkImageLayout img_layout;
    uint64_t last_ns;
    uint64_t accum_ns;
    uint32_t accum_frames;
    double shown_fps;
    int dirty;
} F;

// 5x7 bitmap font for '0'-'9', 'F', 'P', 'S', '.', ' '
static const uint8_t *glyph_5x7(char c) {
    static const uint8_t g_space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t g_dot[7]   = {0, 0, 0, 0, 0, 0x0C, 0x0C};
    static const uint8_t g_0[7]     = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static const uint8_t g_1[7]     = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t g_2[7]     = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static const uint8_t g_3[7]     = {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E};
    static const uint8_t g_4[7]     = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static const uint8_t g_5[7]     = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
    static const uint8_t g_6[7]     = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static const uint8_t g_7[7]     = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const uint8_t g_8[7]     = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static const uint8_t g_9[7]     = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
    static const uint8_t g_F[7]     = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t g_P[7]     = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t g_S[7]     = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    switch (c) {
    case '0': return g_0;
    case '1': return g_1;
    case '2': return g_2;
    case '3': return g_3;
    case '4': return g_4;
    case '5': return g_5;
    case '6': return g_6;
    case '7': return g_7;
    case '8': return g_8;
    case '9': return g_9;
    case 'F': return g_F;
    case 'P': return g_P;
    case 'S': return g_S;
    case '.': return g_dot;
    default:  return g_space;
    }
}

static void render_fps_bitmap(double fps) {
    uint32_t *px = (uint32_t *)F.mapped;
    if (!px) return;
    // Dark background (RGBA8)
    for (int i = 0; i < FPS_W * FPS_H; i++) px[i] = 0xD014161Au;

    char text[32];
    snprintf(text, sizeof(text), "%.1f FPS", fps);
    int len = (int)strlen(text);
    int scale = 2;
    int char_w = 6 * scale;
    int total_w = len * char_w;
    int start_x = (FPS_W - total_w) / 2;
    if (start_x < 4) start_x = 4;
    int start_y = (FPS_H - 7 * scale) / 2;

    uint32_t fg = (fps >= 55.0) ? 0xFF66FF88u : (fps >= 30.0 ? 0xFF44CCFFu : 0xFF5555FFu);
    for (int ci = 0; ci < len; ci++) {
        const uint8_t *g = glyph_5x7(text[ci]);
        int ox = start_x + ci * char_w;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(g[row] & (1 << (4 - col)))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int x = ox + col * scale + sx;
                        int y = start_y + row * scale + sy;
                        if ((unsigned)x < FPS_W && (unsigned)y < FPS_H)
                            px[y * FPS_W + x] = fg;
                    }
                }
            }
        }
    }
}

static int find_mem_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return (int)i;
    }
    return -1;
}

static int ensure_init(VkDevice dev, VkPhysicalDevice phys) {
    if (F.init) return !F.failed;
    F.init = 1;
    if (!dev || !phys) { F.failed = 1; return 0; }

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = FPS_W * FPS_H * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bci, NULL, &F.buf) != VK_SUCCESS) { F.failed = 1; return 0; }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, F.buf, &req);
    int mt = find_mem_type(phys, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { F.failed = 1; return 0; }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(dev, &mai, NULL, &F.buf_mem) != VK_SUCCESS ||
        vkBindBufferMemory(dev, F.buf, F.buf_mem, 0) != VK_SUCCESS ||
        vkMapMemory(dev, F.buf_mem, 0, VK_WHOLE_SIZE, 0, &F.mapped) != VK_SUCCESS) {
        F.failed = 1;
        return 0;
    }

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {FPS_W, FPS_H, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev, &ici, NULL, &F.img) != VK_SUCCESS) { F.failed = 1; return 0; }
    vkGetImageMemoryRequirements(dev, F.img, &req);
    mt = find_mem_type(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { F.failed = 1; return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(dev, &mai, NULL, &F.img_mem) != VK_SUCCESS ||
        vkBindImageMemory(dev, F.img, F.img_mem, 0) != VK_SUCCESS) {
        F.failed = 1;
        return 0;
    }

    F.img_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    F.last_ns = plat_now_ns();
    F.shown_fps = 60.0;
    F.dirty = 1;
    render_fps_bitmap(F.shown_fps);
    return 1;
}

void vk_fps_draw(VkDevice dev, VkPhysicalDevice phys, VkCommandBuffer cb, VkImage dst,
                 VkImageLayout *dst_layout, VkPipelineStageFlags2 *last_stage,
                 VkAccessFlags2 *last_write, uint32_t sc_w, uint32_t sc_h) {
    const char *mode = cfg_get("PERF_OVERLAY");
    if (!mode || strcmp(mode, "off") == 0) return;
    if (sc_w < FPS_W + 32 || sc_h < FPS_H + 32) return;
    if (!ensure_init(dev, phys)) return;

    uint64_t now = plat_now_ns();
    if (F.last_ns && now > F.last_ns) {
        F.accum_ns += now - F.last_ns;
        F.accum_frames++;
        if (F.accum_ns >= 250000000ULL) {
            F.shown_fps = (double)F.accum_frames * 1e9 / (double)F.accum_ns;
            F.accum_ns = 0;
            F.accum_frames = 0;
            render_fps_bitmap(F.shown_fps);
            F.dirty = 1;
        }
    }
    F.last_ns = now;

    if (F.dirty) {
        VkImageMemoryBarrier2 b_dst = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = F.img_layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = F.img,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b_dst};
        vkCmdPipelineBarrier2(cb, &dep);

        VkBufferImageCopy bic = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {FPS_W, FPS_H, 1},
        };
        vkCmdCopyBufferToImage(cb, F.buf, F.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

        b_dst.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b_dst.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b_dst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b_dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier2(cb, &dep);
        F.img_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        F.dirty = 0;
    }

    VkImageMemoryBarrier2 b_sc = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = *last_stage,
        .srcAccessMask = *last_write,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = *dst_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = dst,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dep_sc = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b_sc};
    vkCmdPipelineBarrier2(cb, &dep_sc);

    int32_t x0 = (int32_t)sc_w - FPS_W - 16;
    int32_t y0 = 16;
    VkImageBlit region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {FPS_W, FPS_H, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{x0, y0, 0}, {x0 + FPS_W, y0 + FPS_H, 1}},
    };
    vkCmdBlitImage(cb, F.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    *dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    *last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    *last_write = VK_ACCESS_2_TRANSFER_WRITE_BIT;
}
