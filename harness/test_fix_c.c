// test_fix_c.c - C-level checks of the syndrome machinery, run against the
// real fixers in mode-s.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mode-s.h"
extern uint32_t mode_s_checksum(unsigned char *msg, int bits);
extern int fix_single_bit_errors(unsigned char *msg, int bits);
extern int fix_two_bits_errors(unsigned char *msg, int bits);

static uint32_t stored(const unsigned char *m, int bits)
{
    int n = bits / 8;
    return ((uint32_t)m[n - 3] << 16) | ((uint32_t)m[n - 2] << 8) | m[n - 1];
}
static void forge(unsigned char *m, int bits) // make the parity field valid
{
    int n = bits / 8;
    uint32_t c;
    m[n - 3] = m[n - 2] = m[n - 1] = 0;
    c = mode_s_checksum(m, bits);
    m[n - 3] = c >> 16; m[n - 2] = (c >> 8) & 0xFF; m[n - 1] = c & 0xFF;
}
static void flip(unsigned char *m, int j) { m[j / 8] ^= 1 << (7 - (j % 8)); }

int main(void)
{
    srand(0x1090);
    // 1) syndrome == 0  <=>  stored CRC == historical checksum (112 and 56 bit)
    for (int bits = 56; bits <= 112; bits += 56)
        for (int t = 0; t < 20000; t++)
        {
            unsigned char m[14];
            for (int i = 0; i < bits / 8; i++) m[i] = rand() & 0xFF;
            if (t & 1) forge(m, bits);
            if ((mode_s_crc_syndrome(m, bits) == 0) != (stored(m, bits) == mode_s_checksum(m, bits)))
            { printf("FAIL validity bits=%d\n", bits); return 1; }
        }
    // 2) every single-bit and every double-bit error in a 112-bit frame is
    //    located exactly (exhaustive over positions, random frames)
    for (int t = 0; t < 40; t++)
    {
        unsigned char o[14], m[14];
        for (int i = 0; i < 11; i++) o[i] = rand() & 0xFF;
        forge(o, 112);
        for (int j = 0; j < 112; j++)
        {
            memcpy(m, o, 14); flip(m, j);
            if (fix_single_bit_errors(m, 112) != j || memcmp(m, o, 14)) { printf("FAIL 1bit j=%d\n", j); return 1; }
            if (fix_two_bits_errors(m, 112) != -1) { printf("FAIL: valid frame 'fixed' by 2-bit\n"); return 1; }
            for (int k = j + 1; k < 112; k++)
            {
                memcpy(m, o, 14); flip(m, j); flip(m, k);
                if (fix_single_bit_errors(m, 112) != -1) { printf("FAIL: 2-bit error taken for 1-bit\n"); return 1; }
                if (fix_two_bits_errors(m, 112) != (j | (k << 8)) || memcmp(m, o, 14))
                { printf("FAIL 2bit j=%d k=%d\n", j, k); return 1; }
            }
        }
    }
    // 3) every single-bit error in a 56-bit frame; 2-bit refuses short frames
    for (int t = 0; t < 2000; t++)
    {
        unsigned char o[7], m[7];
        for (int i = 0; i < 4; i++) o[i] = rand() & 0xFF;
        forge(o, 56);
        for (int j = 0; j < 56; j++)
        {
            memcpy(m, o, 7); flip(m, j);
            if (fix_single_bit_errors(m, 56) != j || memcmp(m, o, 7)) { printf("FAIL 1bit56 j=%d\n", j); return 1; }
        }
        memcpy(m, o, 7); flip(m, 3); flip(m, 40);
        if (fix_two_bits_errors(m, 56) != -1) { printf("FAIL: 2-bit accepted a short frame\n"); return 1; }
    }
    printf("OK: validity equivalence (40k frames), exhaustive 1-bit and 2-bit location "
           "(40 frames x 6328 patterns), 56-bit single-bit (2000 frames x 56)\n");
    return 0;
}
