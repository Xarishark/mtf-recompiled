// Amiibo loader and state management subsystem (540-byte NTAG215 binary dumps).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMIIBO_DUMP_SIZE 540

// Device state corresponding to nn::nfp / nn::nfc DeviceState
typedef enum {
    AMIIBO_DEV_STATE_INITIALIZED = 0,
    AMIIBO_DEV_STATE_SEARCHING   = 1,
    AMIIBO_DEV_STATE_TAG_FOUND   = 2,
    AMIIBO_DEV_STATE_TAG_REMOVED = 3,
    AMIIBO_DEV_STATE_TAG_MOUNTED = 4,
    AMIIBO_DEV_STATE_UNAVAILABLE = 5
} AmiiboDeviceState;

// Amiibo identification info
typedef struct {
    uint8_t uuid[10];
    uint8_t uuid_len;
    uint8_t character_id[3];
    uint8_t series_id;
    uint16_t numbering_id;
    uint8_t nfp_type;
    uint8_t version;
    char path[512];
    char filename[128];
} AmiiboInfo;

// Subsystem lifecycle

// Hardware / device emulation status
bool amiibo_is_device_present(void);

// Tag detection & lifecycle
void amiibo_start_detection(void);
void amiibo_stop_detection(void);

// Tag mounting
bool amiibo_mount(void);
void amiibo_unmount(void);
bool amiibo_is_mounted(void);

// State query
AmiiboDeviceState amiibo_get_device_state(void);
// The state the menu shows: searching also within a second after a stop, since the game restarts detection many times
// a second while it waits for a tag. The game reads amiibo_get_device_state.

// File loading & triggering actions

// Timer tick called during menu / frame updates

// Options

// Tag inspection
bool amiibo_has_tag(void);

// Horizon OS buffers extraction (called by NFP IPC/override services)
void amiibo_get_tag_info(void *out_tag_info);
void amiibo_get_model_info(void *out_model_info);
void amiibo_get_common_info(void *out_common_info);
void amiibo_get_register_info(void *out_register_info);
int amiibo_get_application_area(void *out_buf, uint32_t max_size);

#ifdef __cplusplus
}
#endif
