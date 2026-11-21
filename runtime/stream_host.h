// Clean-room browser streaming host (HTTP + WebSocket + WebCodecs VP8/Opus + TryCloudflare)
// and remote gamepad/keyboard input receiver for P2-P4.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STREAM_STATE_OFF = 0,
    STREAM_STATE_STARTING = 1,
    STREAM_STATE_LOCAL_ONLY = 2,
    STREAM_STATE_LIVE = 3,
    STREAM_STATE_ERROR = 4,
};

typedef struct {
    int state;
    int clients;
    int port;
    int cf_installed;
    int cf_installing;
    char code[16];
    char local_url[128];
    char public_url[256];
    char status_msg[160];
    char cf_install_msg[128];
    uint64_t frames_sent;
    uint64_t bytes_sent;
} StreamHostInfo;

int stream_host_has_cloudflared(void);
int stream_host_install_cloudflared(void);
int stream_host_start(void);
void stream_host_stop(void);
int stream_host_active(void);
void stream_host_info(StreamHostInfo *out);

void stream_host_push_video(const uint8_t *bgra, int w, int h, int stride);
void stream_host_push_audio(const float *pcm_interleaved, uint32_t frames, int channels, int rate);
int stream_host_remote_connected(int player);
int stream_host_remote_poll(int player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

#ifdef __cplusplus
}
#endif
