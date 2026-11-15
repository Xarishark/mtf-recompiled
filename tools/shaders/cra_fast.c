/* Fast CRC32/CRC32C block decryptor for Camelot rom:/42 (MTF Recomp, MIT). */
#include <stdint.h>
#include <stddef.h>

static uint32_t crc32_tab[256];
static uint32_t crc32c_tab[256];
static int tables_init = 0;

static void init_tables(void) {
    if (tables_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c1 = i, c2 = i;
        for (int k = 0; k < 8; k++) {
            c1 = (c1 & 1) ? ((c1 >> 1) ^ 0xEDB88320u) : (c1 >> 1);
            c2 = (c2 & 1) ? ((c2 >> 1) ^ 0x82F63B78u) : (c2 >> 1);
        }
        crc32_tab[i] = c1;
        crc32c_tab[i] = c2;
    }
    tables_init = 1;
}

static inline uint32_t crc_u64(const uint32_t *tab, uint32_t c, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        c = tab[(c ^ (uint32_t)(val & 0xFFu)) & 0xFFu] ^ (c >> 8);
        val >>= 8;
    }
    return c;
}

static const uint64_t CASE_MUL_HI[8] = {
    0x11111111ull, 0x22222223ull, 0x33333333ull, 0x44444445ull,
    0x55555555ull, 0x66666667ull, 0x77777777ull, 0x88888889ull,
};

__declspec(dllexport) uint32_t cra_decrypt_blocks(
    uint8_t *buf, size_t aligned_len, size_t raw_size, uint32_t x26
) {
    init_tables();
    uint32_t c = x26 & 7u;
    const uint32_t *tab = (c < 4u) ? crc32_tab : crc32c_tab;
    uint64_t x22 = 1ull | (((uint64_t)((x26 & 0xFFFFFFFEu) ^ (uint32_t)CASE_MUL_HI[c])) << 32);
    int swap_in = ((c & 2u) == 0u);
    int odd_out = ((c & 1u) != 0u);
    size_t nblk = (raw_size / 16u) * 16u;
    uint64_t x23 = 0;

    for (size_t o = 0; o < aligned_len; o += 16) {
        uint64_t a, b;
        __builtin_memcpy(&a, buf + o, 8);
        __builtin_memcpy(&b, buf + o + 8, 8);
        if (swap_in) {
            uint64_t t = a; a = b; b = t;
        }
        uint64_t x0 = (uint64_t)crc_u64(tab, x26, b) * x22;
        uint64_t b2 = x0 ^ ~(b << 32);
        uint64_t a2 = a ^ (x0 >> 3);
        if (odd_out) {
            __builtin_memcpy(buf + o, &a2, 8);
            __builtin_memcpy(buf + o + 8, &b2, 8);
        } else {
            __builtin_memcpy(buf + o, &b2, 8);
            __builtin_memcpy(buf + o + 8, &a2, 8);
        }
        if (o < nblk) {
            x23 += a2;
        }
    }
    return (uint32_t)((x23 >> 32) ^ x23);
}
