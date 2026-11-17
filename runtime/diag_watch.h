// MTF_WATCH: which code writes a block of guest memory, and when, against the commands that read it (diag_watch.c).
#pragma once
#include <stdint.h>

extern int diag_watch_on;   // MTF_WATCH is set: armed, or (c<n>) waiting for the bind that arms it

void diag_watch_init(void);                          // main: after the modules are linked, before the game starts
int diag_watch_hits(uint64_t addr, uint64_t size);  // [addr, addr + size) reaches a watched byte
static inline int diag_watch_range(uint64_t addr, uint64_t size) { return diag_watch_on && diag_watch_hits(addr, size); }
// One timeline line ("watch: f<frame> s<submit> ..."), inside the logging window only.
void diag_watch_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_watch_uniform(int index, uint64_t addr, uint64_t size);   // a uniform bind (MTF_WATCH=c<n> arms on it)
void diag_watch_mark_cmd(const void *cmd);           // a command buffer recorded a command that reaches the range
void diag_watch_end_cmd(const void *cmd, uint64_t handle);   // EndRecording: logs a marked buffer's handle
void diag_watch_submit(const uint64_t *handles, int n);      // QueueSubmitCommands, before the replay
void diag_watch_key(void);                           // F12 in the game window (MTF_WATCH_KEY=1)
