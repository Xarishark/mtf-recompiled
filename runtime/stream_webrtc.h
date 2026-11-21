// Clean-room WebRTC peer manager (libdatachannel) for low-latency UDP media + input DataChannels.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*WebRtcSignalCb)(void *user, const char *json_msg);
typedef void (*WebRtcInputCb)(void *user, int slot, const uint8_t *data, size_t len);
typedef void (*WebRtcStateCb)(void *user, int slot, int connected);

void webrtc_init(void);
void webrtc_shutdown(void);

// Creates a PeerConnection for client slot (0..2), generates SDP offer, and emits signaling JSON via sig_cb.
int webrtc_peer_start(int slot, const char *stun_url, const char *turn_url,
                      WebRtcSignalCb sig_cb, void *sig_user,
                      WebRtcInputCb in_cb, WebRtcStateCb st_cb, void *cb_user);
void webrtc_peer_handle_signal(int slot, const char *json_msg);
void webrtc_peer_close(int slot);
int webrtc_peer_is_open(int slot);
void webrtc_broadcast_media(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
