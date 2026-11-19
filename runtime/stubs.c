// Host subsystems that are not ported yet. Each stand-in names what it replaces; one that a guest call would need
// real behaviour from is fatal with that name, so the boot stops where the missing piece is first needed.
#include "audren.h"
#include "fs.h"
#include "host.h"

// ---- profiling: not ported
void perf_main_thread(void) {}

// ---- game clock: this port has no world clock yet, so MTF_CRIT world windows never open
int64_t frame_step_world_us(void) { return -1; }
