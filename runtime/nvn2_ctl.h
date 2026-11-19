// NVN shader control sections, NVN1 (Switch) and NVN2 (Switch 2). NVN1's: the NVN stage at
// +0x714, c1's offset in the code area at +0x700 and the code area's length at +0x704. An NVN2 section starts with the
// same magic and version 0xc9 at +4; its c1 offset is at +0x40 and the code area's length at +0x44 (c1 runs to the
// end of the area). Its stage: the byte at +0x56 for compute, task and mesh (5, 6, 7), otherwise the shader program
// header at the start of the code (type in bits 10..13 of word 0).
//
// An NVN2 control section is only guaranteed valid during nvnProgramSetShaders, so the program keeps a
// copy (nvn_api_obj.c): NVN2_CTL_COPY bytes, then the shader's key once it is known (gfx_shader.c).
#pragma once
#include <stdint.h>
#include <string.h>

#define NVN_CTL_MAGIC 0x98761234u
#define NVN2_CTL_VERSION 0xc9u
#define NVN2_KEY_CTL 0x48           // the control bytes in an NVN2 shader's key, +0x30..+0x34 hashed as zero
#define NVN2_CTL_COPY 0x100         // bytes of the control section a program keeps
#define NVN2_KEY_STATE 16           // after the copy: state (0 unknown, 1 known, 2 no prebuilt shader), the 8-byte key

static inline int nvn2_ctl(const uint8_t *c) {
    uint32_t magic, ver;
    memcpy(&magic, c, 4), memcpy(&ver, c + 4, 4);
    return magic == NVN_CTL_MAGIC && ver == NVN2_CTL_VERSION;
}

static inline uint32_t nvn_ctl_u32(const uint8_t *c, uint32_t off) {
    uint32_t v;
    memcpy(&v, c + off, 4);
    return v;
}

// The code area's length and c1's offset in it
static inline uint32_t nvn_ctl_len(const uint8_t *c) { return nvn_ctl_u32(c, nvn2_ctl(c) ? 0x44 : 0x704); }
static inline uint32_t nvn_ctl_c1(const uint8_t *c) { return nvn_ctl_u32(c, nvn2_ctl(c) ? 0x40 : 0x700); }

// The NVN stage (0 vertex, 1 fragment, 2 geometry, 3 tessellation control, 4 evaluation, 5 compute); -1 for a task or
// mesh shader, or an NVN2 graphics shader whose code is not mapped yet
static inline int nvn_ctl_stage(const uint8_t *c, const uint8_t *code) {
    if (!nvn2_ctl(c)) return (int)nvn_ctl_u32(c, 0x714);
    if (c[0x56] == 5) return 5;
    if (c[0x56] == 6 || c[0x56] == 7 || !code) return -1;
    static const int8_t by_sph[16] = {-1, 0, 3, 4, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    return by_sph[(nvn_ctl_u32(code, 0) >> 10) & 0xf];
}
