// Settings: the MTF_* environment variables, with mtf.ini next to mtf-recomp.exe behind them (cfg.c).
#pragma once

// getenv for a setting: the environment variable when set, else the mtf.ini key (the name without "MTF_", any case),
// else NULL. A name without the MTF_ prefix goes to getenv only. Safe from any thread once cfg_init has returned.
const char *cfg_get(const char *name);

// Reads mtf.ini once, from mem_init after the address space is claimed and before any setting is read.
void cfg_init(void);

// Replaces the loaded keys with those of the file at path (tests). 1 when the file was read.

int cfg_from_env(const char *name);   // the environment sets name

// The value for consumers: cfg_get, else the runtime default from cfg.c, else "". cfg_int / cfg_double parse it (an
// unparsable value gives 0); cfg_bool: "" and "0" are false.
const char *cfg_value(const char *name);
int cfg_int(const char *name);
double cfg_double(const char *name);
int cfg_bool(const char *name);
unsigned cfg_version(void);           // bumped by every cfg_set (the in-game menu, F11)
void cfg_set(const char *name, const char *value);   // in memory until cfg_save; the environment still wins
int cfg_save(void);                    // writes mtf.ini beside the executable; 1 on success
int cfg_last_save_ok(void);            // the last cfg_save wrote (1 before any)

// What the menu knows about a setting. key: without MTF_, upper case. def: what the consumer uses when the key is unset
// ("" = no fixed default, the consumer picks per machine). values: "0|1", "mailbox|fifo|immediate", a range "0..300",
// or NULL for free text. live: applies at once; the others are read at start.
typedef struct { const char *key, *def, *values; int live; } CfgMeta;
const CfgMeta *cfg_meta(const char *key);   // prefix and case ignored; NULL for a key the menu does not show
const CfgMeta *cfg_meta_at(int i);          // the i-th row (table order); NULL past the last
// The keys back to their defaults (unset; the environment's still win); the number of keys that had a value.
int cfg_reset(const char *const *keys, int n);
