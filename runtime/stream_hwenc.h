// Hardware H.264 / AV1 video encoder via Windows Media Foundation (IMFTransform).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STREAM_CODEC_VP8 = 0,
    STREAM_CODEC_H264 = 1,
    STREAM_CODEC_AV1 = 2,
};

// Probes available hardware MFT encoders on the host GPU (bitmask: bit 1 = H.264, bit 2 = AV1).
int stream_hwenc_probe(void);

// Opens hardware encoder for codec (STREAM_CODEC_H264 or STREAM_CODEC_AV1). Returns 1 on success, 0 on failure.
int stream_hwenc_open(int codec, int w, int h, int fps, int kbps, int kf_dist);

// Encodes one NV12 frame (tightly packed Y plane w*h followed by interleaved UV plane w*h/2).
// *out_buf points to an internal buffer valid until the next encode/close call. Returns 1 if a frame was produced.
int stream_hwenc_encode(const uint8_t *nv12, int w, int h, int force_kf,
                        const uint8_t **out_buf, size_t *out_len, int *out_kf);

void stream_hwenc_close(void);

#ifdef __cplusplus
}
#endif
