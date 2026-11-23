#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int available;
    char tag[32];
    char url[256];
} UpdateInfo;

// Starts the background GitHub release check once (no-op if MTF_UPDATE_CHECK=0).
void update_check_start(void);

// Copies the current update status into *out; returns out->available (0 or 1).
int update_check_status(UpdateInfo *out);

// Opens the latest release page in the user's default browser.
void update_check_open_url(void);

#ifdef __cplusplus
}
#endif
