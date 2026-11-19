// rv_main.c - the firmware's decoder with a piece of signal linked in, for
// counting RISC-V instructions under qemu-riscv32 (cost_rv32.py builds and
// runs it). No stdio and no heap: the toolchain's C library is the bare
// one the firmware uses, and under qemu's Linux emulation only read/write
// and exit can be counted on.
#include <string.h>
#include <unistd.h>
#include <reent.h>
#include "mode-s.h"

struct _reent *__getreent(void) { return _GLOBAL_REENT; } // the library asks the RTOS for this

extern const unsigned char sig_start[], sig_end[];
__asm__(".section .rodata\n.global sig_start\n.global sig_end\nsig_start:\n.incbin \"" SIGFILE "\"\nsig_end:\n.text\n");

static unsigned n_msgs;
static void on_msg(mode_s_t *s, struct mode_s_msg *mm)
{
    (void)s;
    (void)mm;
    n_msgs++;
}
static mode_s_t st;
#define CHUNK 45056 // 8 USB transfers of 5632 bytes: what the decoder task usually takes at once

int main(int argc, char **argv)
{
    size_t len = (size_t)(sig_end - sig_start), off;
    static uint16_t mag[CHUNK / 2 + MODE_S_STREAM_TAIL_MAX];
    static unsigned char buf[CHUNK];
    static unsigned char coh_mem[262144];
    int decode = argc < 2 || strcmp(argv[1], "init"); // "init": start-up only, to subtract
    mode_s_init(&st);
    // the firmware's default decoder (class_driver.c, DEC_BOOT_FIX / DEC_BOOT_COH)
    st.strict_es = 1;
    st.fix2_known_icao = 1;
    st.soft_fix = 8;
    if (mode_s_coh_work_size() > sizeof(coh_mem))
        return 9;
    mode_s_coh_enable(&st, coh_mem, 1);
#ifdef MODE_S_LVL_BINS
    {
        static uint32_t hist[MODE_S_LVL_BINS];
        st.lvl_hist = hist;
    }
#endif
    if (decode)
        for (off = 0; off + 2 <= len; off += CHUNK)
        {
            size_t n = len - off < CHUNK ? len - off : CHUNK;
            memcpy(buf, sig_start + off, n);
            mode_s_detect_stream_2400(&st, buf, (uint32_t)(n & ~(size_t)1), mag, on_msg);
        }
    {
        // "<frames delivered> <CRC-ok> <candidates>\n"
        char t[48];
        int k = 0;
        unsigned v[3] = {n_msgs, st.stat_crc_ok, st.stat_preambles};
        for (int j = 0; j < 3; j++)
        {
            char d[12];
            int n = 0;
            unsigned x = v[j];
            do
            {
                d[n++] = (char)('0' + x % 10);
                x /= 10;
            } while (x);
            while (n)
                t[k++] = d[--n];
            t[k++] = j < 2 ? ' ' : '\n';
        }
        (void)!write(1, t, (size_t)k);
    }
    return 0;
}
