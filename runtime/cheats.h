#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Applies or restores active cheat toggles in guest memory once nro0 .bss is mapped.
void cheats_tick(void);

// Sets all cheat toggles ON or OFF in cfg (live).
void cheats_set_all(int enable);

// Returns 1 if nro0's unlock bitfield is mapped and writable in this process.
int cheats_available(void);

#ifdef __cplusplus
}
#endif
