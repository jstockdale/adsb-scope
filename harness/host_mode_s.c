// host_mode_s.c - host-side driver for the firmware's mode-s.c decoder.
//
// Feeds a cu8 file (interleaved uint8 I,Q, offset 128 - the RTL-SDR native
// format) through the firmware decoder, chunked like class_driver.c does.
//
//   --stream 0   historical firmware path: per chunk,
//                mode_s_compute_magnitude_vector() + mode_s_detect().
//                Frames straddling a chunk boundary are lost.
//   --stream 1   new firmware path: mode_s_detect_stream() per chunk
//                (carries a tail; identical to one-buffer processing).
//
// Output: one line per delivered frame
//   MSG samp=<abs sample index of preamble> df=.. bits=.. crcok=..
//       err=<errorbit or -1> delta=<bit delta> hex=<frame>
// and a STATS line on stderr.
//
// Built two ways (see Makefile):
//   host_mode_s           the current decoder (../main/examples/adsb_receiver)
//   host_mode_s_baseline  the pristine historical decoder (baseline/), -DBASELINE
//   host_mode_s_df18off   current decoder with -DMODE_S_DF18_FIX=0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mode-s.h"

#ifdef BASELINE
#define MODE_S_STREAM_TAIL_MAX 0
#endif

static mode_s_t st;
static uint64_t g_base = 0; // abs sample index of mag[0] (legacy path)
static int g_stream = 0;
static int g_rate24 = 0;    // 1: input is 2.4 Msps, use the 2.4 Msps demodulator
static uint32_t g_pos_msgs = 0; // DF17/18 position messages delivered (surface or airborne)
#ifndef BASELINE
static uint32_t g_lvl[MODE_S_LVL_BINS]; // level histogram, as the firmware keeps for gain control
#endif

static void on_msg(mode_s_t *self, struct mode_s_msg *mm)
{
    uint64_t samp;
#ifndef BASELINE
    if (g_stream)
        samp = mode_s_frame_sample_index(self);
    else
#endif
        samp = g_base + self->last_frame_offset;
    if (mm->crcok && (mm->msgtype == 17 || mm->msgtype == 18) &&
        ((mm->metype >= 5 && mm->metype <= 18) || (mm->metype >= 20 && mm->metype <= 22)))
        g_pos_msgs++;
    printf("MSG samp=%llu df=%d bits=%d crcok=%d err=%d delta=%u ",
           (unsigned long long)samp, mm->msgtype, mm->msgbits, mm->crcok,
           mm->errorbit, self->last_delta);
#ifndef BASELINE
    if (g_rate24) // preamble start in 12 MHz ticks (the Beast timestamp clock)
        printf("tick=%llu ", (unsigned long long)(samp * 5 + self->last_frame_phase));
#endif
    printf("hex=");
    for (int i = 0; i < mm->msgbits / 8; i++)
        printf("%02X", mm->msg[i]);
    printf("\n");
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s file.cu8 [--chunk BYTES] [--stream 0|1]\n"
            "       [--fix 0|1] [--aggressive 0|1] [--check-crc 0|1]\n"
#ifndef BASELINE
            "       [--fix2 0|1] [--strict 0|1] [--known-icao 0|1]\n"
            "       [--fix2-delta N] [--df18 0|1]\n"
            "       [--rate 2.0|2.4]   2.4 selects the 2.4 Msps demodulator, with\n"
            "       [--gate N] [--gate2 N] [--pre N] [--tries N] [--soft N]  (its tuning; see mode-s.h)\n"
            "       [--coh 0|1] [--coh-ts N] [--coh-gate N] [--coh-df 0|1|2]  coherent second look (mode_s_coh.c)\n"
            "       [--coh-on-at BYTE] [--coh-off-at BYTE]   switch it between two chunks, as the firmware does\n"
            "       [--win SECONDS]   a WIN line on stderr per window of signal: what the gain\n"
            "                         control is given (stream mode; counters restart per window)\n"
#endif
            "defaults: chunk=262144 (the firmware's DEFAULT_BUF_LENGTH), stream=0,\n"
            "          fix=1, everything else 0 except --df18 1 (deliver DF18 so it\n"
            "          can be scored; the firmware default is 0)\n",
            a0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    size_t chunk = 262144;
    int fix = 1, aggressive = 0, check_crc = 1;
    int fix2 = 0, strict = 0, known = 0, df18 = 1;
    uint32_t fix2_delta = 0;
    int gate = -1, gate2 = -1, pre = -1, tries = -1, soft = -1;
    int coh = 0, coh_ts = -1, coh_gate = -1, coh_df = -1;
    long coh_on_at = -1, coh_off_at = -1; // switch the look at the first chunk boundary at or after this byte
    // One loss of samples in the stream (stream mode only): at byte offset
    // gap_at of the input, gap_skip bytes are thrown away and the decoder
    // is told gap_lost samples went missing.
    long gap_at = -1, gap_skip = 0;
    unsigned long long gap_lost = 0;
    int cache_clock = 0; // 1: the ICAO cache ages by samples, as in the device's replay mode
    double win_s = 0;    // > 0: print gain-control windows of this many seconds of signal

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (a[0] != '-')
        {
            path = a;
            continue;
        }
        if (!v)
        {
            usage(argv[0]);
            return 2;
        }
        i++;
        if (!strcmp(a, "--chunk"))
            chunk = (size_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--stream"))
            g_stream = atoi(v);
        else if (!strcmp(a, "--fix"))
            fix = atoi(v);
        else if (!strcmp(a, "--aggressive"))
            aggressive = atoi(v);
        else if (!strcmp(a, "--check-crc"))
            check_crc = atoi(v);
        else if (!strcmp(a, "--fix2"))
            fix2 = atoi(v);
        else if (!strcmp(a, "--strict"))
            strict = atoi(v);
        else if (!strcmp(a, "--known-icao"))
            known = atoi(v);
        else if (!strcmp(a, "--fix2-delta"))
            fix2_delta = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--df18"))
            df18 = atoi(v);
        else if (!strcmp(a, "--rate"))
            g_rate24 = !strcmp(v, "2.4");
        else if (!strcmp(a, "--cache-clock"))
            cache_clock = atoi(v);
        else if (!strcmp(a, "--gap-at"))
            gap_at = strtol(v, NULL, 0);
        else if (!strcmp(a, "--gap-skip"))
            gap_skip = strtol(v, NULL, 0);
        else if (!strcmp(a, "--gap-lost"))
            gap_lost = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--gate"))
            gate = atoi(v);
        else if (!strcmp(a, "--gate2"))
            gate2 = atoi(v);
        else if (!strcmp(a, "--pre"))
            pre = atoi(v);
        else if (!strcmp(a, "--tries"))
            tries = atoi(v);
        else if (!strcmp(a, "--soft"))
            soft = atoi(v);
        else if (!strcmp(a, "--coh"))
            coh = atoi(v);
        else if (!strcmp(a, "--coh-ts"))
            coh_ts = atoi(v);
        else if (!strcmp(a, "--coh-gate"))
            coh_gate = atoi(v);
        else if (!strcmp(a, "--coh-df"))
            coh_df = atoi(v);
        else if (!strcmp(a, "--coh-on-at"))
            coh_on_at = strtol(v, NULL, 0);
        else if (!strcmp(a, "--coh-off-at"))
            coh_off_at = strtol(v, NULL, 0);
        else if (!strcmp(a, "--win"))
            win_s = atof(v);
        else
        {
            usage(argv[0]);
            return 2;
        }
    }
    if (!path || chunk < 2 || (chunk & 1))
    {
        usage(argv[0]);
        return 2;
    }

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }

    // Never allocate more than the input needs: a very large --chunk just
    // means "the whole file as one buffer".
    if (fseek(f, 0, SEEK_END) == 0)
    {
        long sz = ftell(f);
        if (sz > 0 && (size_t)sz < chunk)
            chunk = ((size_t)sz + 1) & ~(size_t)1;
        fseek(f, 0, SEEK_SET);
    }

    mode_s_init(&st);
    st.fix_errors = fix;
    st.aggressive = aggressive;
    st.check_crc = check_crc;
#ifdef BASELINE
    if (g_stream || fix2 || strict || known || fix2_delta || g_rate24)
    {
        fprintf(stderr, "the baseline decoder has no stream/fix2/strict/known-icao support\n");
        return 2;
    }
    (void)df18; (void)gate; (void)gate2; (void)pre; (void)tries; (void)soft;
    (void)coh; (void)coh_ts; (void)coh_gate; (void)coh_df; (void)coh_on_at; (void)coh_off_at;
    (void)win_s;
#else
    st.lvl_hist = g_lvl; // counted in stream mode; it changes nothing that is decoded
    st.fix2 = fix2;
    st.strict_es = strict;
    st.fix2_known_icao = known;
    st.fix2_min_delta = fix2_delta;
    st.pass_df18 = df18;
    if (gate >= 0) st.d24_gate_x100 = (uint16_t)gate;
    if (gate2 >= 0) st.d24_gate2_x100 = (uint16_t)gate2;
    if (pre >= 0) st.d24_pre_x100 = (uint16_t)pre;
    if (tries >= 0) st.d24_tries = (uint8_t)tries;
    if (soft >= 0) st.soft_fix = (uint8_t)soft;
    void *coh_mem = (coh || coh_on_at >= 0) ? malloc(mode_s_coh_work_size()) : NULL;
    if ((coh || coh_on_at >= 0) && !coh_mem)
    {
        fprintf(stderr, "no memory for the coherent second look\n");
        return 2;
    }
    if (coh)
        mode_s_coh_enable(&st, coh_mem, 1);
    if (coh_ts >= 0) st.coh_tsearch = (uint8_t)coh_ts;
    if (coh_gate >= 0) st.coh_gate_x100 = (uint16_t)coh_gate;
    if (coh_df >= 0) st.coh_df_gate = (uint8_t)coh_df;
#endif

    unsigned char *buf = malloc(chunk);
    uint16_t *mag = malloc((chunk / 2 + MODE_S_STREAM_TAIL_MAX) * sizeof(uint16_t));
    if (!buf || !mag)
    {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

#ifndef BASELINE
    if (cache_clock)
        mode_s_cache_clock(&st, g_rate24 ? 2400000 : 2000000);
#else
    (void)cache_clock;
#endif
    if (gap_at >= 0 && (!g_stream || (gap_at & 1) || (gap_skip & 1)))
    {
        fprintf(stderr, "--gap-at needs --stream 1 and even byte counts\n");
        return 2;
    }

    uint64_t consumed = 0;
    long fed = 0; // bytes of the input handed to the decoder so far
#ifndef BASELINE
    uint64_t win_start = 0;
    static uint32_t tot_lvl[MODE_S_LVL_BINS];
    struct { uint32_t preambles, ok, fail, es_ok, es_lost, loud, pos; } tot = {0};
#endif
    for (;;)
    {
        size_t want = chunk;
#ifndef BASELINE
        if (gap_at >= 0 && fed == gap_at)
        {
            if (fseek(f, gap_skip, SEEK_CUR) != 0)
                break;
            mode_s_stream_gap(&st, gap_lost);
            gap_at = -1;
        }
        if (gap_at >= 0 && fed + (long)want > gap_at)
            want = (size_t)(gap_at - fed); // stop exactly at the loss
        // The firmware changes decoder options between two runs of the
        // decoder (class_driver.c, decoder_apply_options); so does this.
        if (coh_on_at >= 0 && fed >= coh_on_at)
        {
            mode_s_coh_enable(&st, coh_mem, 1);
            coh_on_at = -1;
        }
        if (coh_off_at >= 0 && fed >= coh_off_at)
        {
            mode_s_coh_enable(&st, NULL, 0);
            coh_off_at = -1;
        }
#endif
        size_t got = fread(buf, 1, want, f);
        got &= ~(size_t)1; // whole I/Q pairs
        if (got == 0)
            break;
        fed += (long)got;
#ifndef BASELINE
        if (g_stream)
        {
            if (g_rate24)
                mode_s_detect_stream_2400(&st, buf, (uint32_t)got, mag, on_msg);
            else
                mode_s_detect_stream(&st, buf, (uint32_t)got, mag, on_msg);
        }
        else if (g_rate24)
        {
            g_base = consumed;
            mode_s_compute_magnitude_vector(buf, mag, (uint32_t)got);
            mode_s_detect_2400(&st, mag, (uint32_t)(got / 2), on_msg);
        }
        else
#endif
        {
            // historical demodulate(): no carry between chunks
            g_base = consumed;
            mode_s_compute_magnitude_vector(buf, mag, (uint32_t)got);
            if (got / 2 > 240) // the historical loop bound underflows below one frame
                mode_s_detect(&st, mag, (uint32_t)(got / 2), on_msg);
        }
        consumed += got / 2;
#ifndef BASELINE
        if (win_s > 0)
        {
            // What class_driver.c hands the gain control per window, read
            // and cleared the same way. The totals go on to the STATS line.
            const double rate = g_rate24 ? 2.4e6 : 2.0e6;
            if ((double)(consumed - win_start) >= win_s * rate)
            {
                mode_s_level_t lv;
                mode_s_level_read(g_lvl, &lv);
                fprintf(stderr, "WIN t=%.2f secs=%.3f ok=%u es_ok=%u es_lost=%u loud=%u pos=%u noise=%.3f clip=%.6f "
                                "sig=%u delta=%u cand=%u\n",
                        (double)consumed / rate, (double)(consumed - win_start) / rate,
                        st.stat_crc_ok, st.stat_es_ok, st.stat_es_lost, st.stat_loud, g_pos_msgs,
                        (double)lv.noise, (double)lv.clip,
                        st.stat_crc_ok ? (unsigned)(st.stat_signal_sum / st.stat_crc_ok) : 0u,
                        st.stat_crc_ok ? (unsigned)(st.stat_delta_sum / st.stat_crc_ok) : 0u,
                        st.stat_preambles);
                for (int k = 0; k < MODE_S_LVL_BINS; k++)
                    tot_lvl[k] += g_lvl[k];
                memset(g_lvl, 0, sizeof(g_lvl));
                tot.preambles += st.stat_preambles; st.stat_preambles = 0;
                tot.ok += st.stat_crc_ok; st.stat_crc_ok = 0;
                tot.fail += st.stat_crc_fail; st.stat_crc_fail = 0;
                tot.es_ok += st.stat_es_ok; st.stat_es_ok = 0;
                tot.es_lost += st.stat_es_lost; st.stat_es_lost = 0;
                tot.loud += st.stat_loud; st.stat_loud = 0;
                tot.pos += g_pos_msgs; g_pos_msgs = 0;
                st.stat_signal_sum = 0;
                st.stat_delta_sum = 0;
                win_start = consumed;
            }
        }
#endif
    }
    fclose(f);

#ifndef BASELINE
    for (int k = 0; k < MODE_S_LVL_BINS; k++)
        g_lvl[k] += tot_lvl[k];
    st.stat_preambles += tot.preambles;
    st.stat_crc_ok += tot.ok;
    st.stat_crc_fail += tot.fail;
    st.stat_es_ok += tot.es_ok;
    st.stat_es_lost += tot.es_lost;
    st.stat_loud += tot.loud;
    g_pos_msgs += tot.pos;
#endif
    fprintf(stderr, "STATS preambles=%u crc_ok=%u crc_fail=%u",
            st.stat_preambles, st.stat_crc_ok, st.stat_crc_fail);
#ifndef BASELINE
    fprintf(stderr, " fix1=%u fix2=%u soft=%u fix2_icao_rej=%u strict_rej=%u df18=%u fail_delta_avg=%.0f",
            st.stat_fix1, st.stat_fix2, st.stat_soft_fix, st.stat_fix2_icao_rej, st.stat_strict_rej,
            st.stat_df18,
            st.stat_crc_fail ? (double)st.stat_fail_delta_sum / st.stat_crc_fail : 0.0);
    fprintf(stderr, " coh_gated=%u coh_try=%u coh_abort=%u coh_ok=%u coh_soft=%u",
            st.stat_coh_gated, st.stat_coh_try, st.stat_coh_abort, st.stat_coh_ok, st.stat_coh_soft);
    {
        mode_s_level_t lv;
        mode_s_level_read(g_lvl, &lv);
        fprintf(stderr, " es_ok=%u es_lost=%u loud=%u pos=%u noise=%.3f clip=%.6f",
                st.stat_es_ok, st.stat_es_lost, st.stat_loud, g_pos_msgs, (double)lv.noise, (double)lv.clip);
    }
#endif
    fprintf(stderr, " samples=%llu\n", (unsigned long long)consumed);
    free(buf);
    free(mag);
    return 0;
}
