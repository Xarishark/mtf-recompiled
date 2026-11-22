#pragma once
#include <stdint.h>

// Mario Tennis Fever (0400D4201C6EA000, Update 1.1.1 / v327680) nro0 unlock bitfield layout.
// Base address in nro0 .bss; each bit is addressed MSB-first within its byte:
//   byte[bit >> 3] & (0x80u >> (bit & 7u))

#define MTF_UNLOCK_BITFIELD_VA   0x7005cc09e4ull
#define MTF_UNLOCK_BITFIELD_SIZE 0x300u

typedef struct CheatBitRange {
    uint16_t bit_start;   // inclusive bit index from MTF_UNLOCK_BITFIELD_VA
    uint16_t bit_end;     // inclusive bit index
} CheatBitRange;

// Characters: f_700295c980 (nro_0086.c) checks bit 0x1540 + chara_id for chara_id <= 0x29 (42 slots)
static const CheatBitRange kCheatCharRanges[] = {
    {0x1540u, 0x1569u},
};

// Alternate character costumes / colors: nro_0090.c (charasel) checks 0x7005cc0caa..0xcac (bits 0x1630..0x1647)
static const CheatBitRange kCheatCostumeRanges[] = {
    {0x1630u, 0x1647u},
};

// Rackets: f_700295cad0 (nro_0086.c) & nro_0090.c check bit 0x15d0 + racket_id for racket_id in [0x30..0x5e] (47 rackets)
static const CheatBitRange kCheatRacketRanges[] = {
    {0x1600u, 0x162eu},
};

// Courts: f_700295cc20 (nro_0086.c) & nro_0093.c check bit 0x15d0 + court_id for court_id <= 0x11 (18 courts)
static const CheatBitRange kCheatCourtRanges[] = {
    {0x15d0u, 0x15e1u},
};

// Cups / Modes:
//   0x7005cc0c84 (byte 0x2a0, bits 0x1500..0x1501: MSB bits 7 & 6 -> f_70029bf43c mode flags)
//   0x7005cc0c85 (byte 0x2a1, bit  0x150f: LSB bit 0 -> f_70029bf43c mode flag)
//   0x7005cc0c86 (byte 0x2a2, bits 0x1510..0x1515: MSB bits 7..2 -> Tour / DTour cups)
//   0x7005cc0cae (byte 0x2ca, bit  0x1656: bit 1 -> Tournament unlock)
static const CheatBitRange kCheatModeRanges[] = {
    {0x1500u, 0x1501u},
    {0x150fu, 0x150fu},
    {0x1510u, 0x1515u},
    {0x1656u, 0x1656u},
};
