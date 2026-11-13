// mode_s_2400.c - Mode S demodulator for magnitude samples at 2.4 Msps.
//
// Why a separate demodulator: mode_s_detect() assumes exactly two samples
// per bit and decides each bit by comparing two adjacent samples. A frame
// that arrives about half a sample off the grid is unreadable that way at
// any signal strength (docs/DECODER.md section 5). At 2.4 Msps a bit spans
// 2.4 samples and a half-bit pulse ("chip") spans 1.2, so no single
// alignment is blind, provided the slicer knows where the chips fall.
//
// Model. Timing is quantised to U = 1/5 sample. One microsecond is 12 U and
// a chip is 6 U, so for a frame starting at any multiple of U every chip
// boundary also falls on a multiple of U. The energy of a chip that starts
// at sample i plus f/5 is then exactly a two-sample weighted sum,
//
//     S(i, f) = (5 - f) * m[i] + (1 + f) * m[i + 1]        (weights sum to 6)
//
// and a bit is 1 when its first chip outweighs its second.
//
// Detection, per sample position n:
//   1. Gate (cheap, every sample): the four preamble pulses, taken as
//      two-sample sums that contain them for any sub-sample offset, must
//      each exceed the level of nine samples that are quiet for every
//      offset. Thresholds are ratios, so nothing depends on absolute
//      signal level. The quiet level is kept as a sliding sum.
//   2. Timing: the energy of the four pulse chips is evaluated at the
//      five sub-sample offsets and the offsets are ranked by it. The best
//      ones are then tested properly: their four pulse chips against the
//      twelve chips of the preamble that must be quiet at that alignment.
//   3. The best offsets are sliced and handed to mode_s_decode(), which
//      owns CRC checking, error correction and all of its safeguards.
//   4. Optional soft rescue: a DF17/18 frame that still fails its CRC is
//      repaired if flipping some of its least confident bits (and only
//      those) produces exactly the observed CRC syndrome.
//
// Written for this project; it shares no code with other decoders.

#include <string.h>
#include "mode-s.h"
#include "mode_s_syndromes.h"

#define U_PER_US 12                 // fifths of a sample per microsecond
#define PREAMBLE_U (8 * U_PER_US)   // preamble length
#define LONG_BITS 112
#define SHORT_BITS 56
#define SOFT_MAX_BITS 8             // most low-confidence bits considered by the soft rescue
#define SOFT_MAX_FLIPS 4            // most bits the soft rescue will flip

// Pulse chip start offsets within the preamble, in U: 0, 1.0, 3.5, 4.5 us.
static const uint8_t PULSE_U[4] = {0, 12, 42, 54};
// ... and the twelve half-microsecond slots of the preamble that are quiet.
static const uint8_t QUIET_U[12] = {6, 18, 24, 30, 36, 48, 60, 66, 72, 78, 84, 90};

// Coarse filter between the gate and the full preamble test: the best
// offset's pulse level must be at least this multiple (x100) of the
// always-quiet level. Deliberately loose; the real test follows.
#define PREFILTER_X100 170u

// (sample offset, fifths) of each preamble chip for each of the 5 timing
// offsets, so candidate evaluation needs no divisions.
static uint8_t t_pi[5][4], t_pf[5][4], t_qi[5][12], t_qf[5][12];
static int t_ready = 0;

static void tables_init(void)
{
    int f, k;
    for (f = 0; f < 5; f++)
    {
        for (k = 0; k < 4; k++)
        {
            t_pi[f][k] = (uint8_t)((f + PULSE_U[k]) / 5);
            t_pf[f][k] = (uint8_t)((f + PULSE_U[k]) % 5);
        }
        for (k = 0; k < 12; k++)
        {
            t_qi[f][k] = (uint8_t)((f + QUIET_U[k]) / 5);
            t_qf[f][k] = (uint8_t)((f + QUIET_U[k]) % 5);
        }
    }
    t_ready = 1;
}

static inline uint32_t chip_sum(const uint16_t *m, uint32_t i, uint32_t f)
{
    return (5 - f) * (uint32_t)m[i] + (1 + f) * (uint32_t)m[i + 1];
}

// Slice bits [from, to) of a frame whose data starts at sample m[0] + f0/5.
// Sets bits in msg, stores |first chip - second chip| per bit in conf, and
// returns the sum of those confidences.
static uint32_t slice_bits(const uint16_t *m, uint32_t f0, int from, int to,
                           unsigned char *msg, uint32_t *conf)
{
    // position of bit 'from': 12 U per bit = 2 samples + 2 fifths
    uint32_t u = f0 + 12u * (uint32_t)from;
    uint32_t i = u / 5, f = u % 5;
    uint32_t sum = 0;
    int k;
    for (k = from; k < to; k++)
    {
        uint32_t ib = i + 1, fb = f + 1; // second chip: +6 U
        int32_t a, b, d;
        uint32_t ad;
        if (fb >= 5)
        {
            fb -= 5;
            ib++;
        }
        a = (int32_t)chip_sum(m, i, f);
        b = (int32_t)chip_sum(m, ib, fb);
        d = a - b;
        if (d > 0)
            msg[k >> 3] |= (unsigned char)(0x80 >> (k & 7));
        ad = (uint32_t)(d < 0 ? -d : d);
        conf[k] = ad;
        sum += ad;
        i += 2;
        f += 2;
        if (f >= 5)
        {
            f -= 5;
            i++;
        }
    }
    return sum;
}

// Slice a whole frame whose preamble starts at sample m[0] + f/5. The DF
// field (first 5 bits) decides the length. Returns the mean confidence per
// bit, scaled to magnitude units (same scale as the 2.0 Msps "bit delta").
static uint32_t slice_frame(const uint16_t *m, uint32_t f, unsigned char *msg,
                            uint32_t *conf, int *nbits)
{
    // data starts PREAMBLE_U after the preamble start: 96 U = 19 samples + 1 fifth
    uint32_t u = f + PREAMBLE_U;
    const uint16_t *d = m + u / 5;
    uint32_t f0 = u % 5;
    uint32_t sum;
    int bits;
    memset(msg, 0, MODE_S_LONG_MSG_BYTES);
    sum = slice_bits(d, f0, 0, 5, msg, conf);
    bits = mode_s_msg_len_by_type(msg[0] >> 3);
    sum += slice_bits(d, f0, 5, bits, msg, conf);
    *nbits = bits;
    return sum / ((uint32_t)bits * 6u);
}

// Soft rescue for a 112-bit frame with a plain CRC (DF17/18) that failed:
// look for a set of at most SOFT_MAX_FLIPS bits, all among the 'width' least
// confident ones, whose combined single-bit syndromes equal the frame's
// syndrome. Prefers the fewest flips. Returns the number of bits flipped
// (msg is corrected in place) or 0.
static int soft_rescue(unsigned char *msg, const uint32_t *conf, int width)
{
    int idx[SOFT_MAX_BITS];
    uint32_t val[SOFT_MAX_BITS];
    uint32_t syn = mode_s_crc_syndrome(msg, LONG_BITS);
    int n = 0, k, j, best_mask = 0, best_flips = SOFT_MAX_FLIPS + 1, mask;

    if (syn == 0)
        return 0;
    if (width > SOFT_MAX_BITS)
        width = SOFT_MAX_BITS;

    // the 'width' least confident bits, by insertion into a small sorted list
    for (k = 0; k < LONG_BITS; k++)
    {
        if (n < width || conf[k] < val[n - 1])
        {
            j = (n < width) ? n++ : n - 1;
            while (j > 0 && val[j - 1] > conf[k])
            {
                val[j] = val[j - 1];
                idx[j] = idx[j - 1];
                j--;
            }
            val[j] = conf[k];
            idx[j] = k;
        }
    }

    for (mask = 1; mask < (1 << n); mask++)
    {
        uint32_t s = 0;
        int flips = 0;
        for (j = 0; j < n; j++)
        {
            if (mask & (1 << j))
            {
                s ^= mode_s_syn_bit_112[idx[j]];
                flips++;
            }
        }
        if (s == syn && flips < best_flips)
        {
            best_flips = flips;
            best_mask = mask;
        }
    }
    if (!best_mask)
        return 0;
    for (j = 0; j < n; j++)
        if (best_mask & (1 << j))
            msg[idx[j] >> 3] ^= (unsigned char)(0x80 >> (idx[j] & 7));
    return best_flips;
}

// Sum of the nine samples after position m that are quiet for every
// sub-sample timing offset: 5..7 (between pulse pairs) and 13..18 (after).
static inline uint32_t quiet9(const uint16_t *m)
{
    return (uint32_t)m[5] + m[6] + m[7] + m[13] + m[14] + m[15] + m[16] + m[17] + m[18];
}

void mode_s_detect_2400(mode_s_t *self, uint16_t *mag, uint32_t maglen, mode_s_callback_t cb)
{
    uint32_t n, limit, q;
    const uint32_t gate = self->d24_gate_x100;
    const uint32_t gate2 = self->d24_gate2_x100;
    const uint32_t pre = self->d24_pre_x100;
    const int tries = self->d24_tries ? self->d24_tries : 1;

    if (!t_ready)
        tables_init();

    // detect_start / detect_resume: same contract as mode_s_detect(), so
    // the streaming wrapper can resume exactly where the last chunk stopped.
    if (maglen <= MODE_S_2400_SPAN)
    {
        self->detect_start = 0;
        self->detect_resume = 0;
        return;
    }
    limit = maglen - MODE_S_2400_SPAN;
    n = self->detect_start;
    if (n < limit)
        q = quiet9(mag + n);
    else
        q = 0;

    while (n < limit)
    {
        const uint16_t *m = mag + n;
        uint32_t t, p, p2, psum;
        uint32_t H[5];
        int order[5];
        int f, k, a;
        unsigned char msg[MODE_S_LONG_MSG_BYTES], first_msg[MODE_S_LONG_MSG_BYTES];
        uint32_t conf[LONG_BITS], first_conf[LONG_BITS];
        uint32_t delta = 0, first_delta = 0, signal = 0;
        int nbits = 0, first_bits = 0, attempted = 0, first_f = 0;
        struct mode_s_msg mm;

        // ---- 1. gate -----------------------------------------------------
        // q is the sum of nine always-quiet samples. A two-sample pulse sum
        // must exceed gate/100 times the quiet level of two samples:
        //   pair > (gate/100) * 2 * (q/9)   <=>   pair * 450 > gate * q
        t = gate * q;
        p = (uint32_t)m[0] + m[1]; // pulse 1
        if (p * 450u <= t)
            goto next;
        psum = p;
        p = (uint32_t)m[11] + m[12]; // pulse 4
        if (p * 450u <= t)
            goto next;
        psum += p;
        p = (uint32_t)m[2] + m[3]; // pulse 2 sits in samples 2..3 or 3..4
        p2 = (uint32_t)m[3] + m[4];
        if (p2 > p)
            p = p2;
        if (p * 450u <= t)
            goto next;
        psum += p;
        p = (uint32_t)m[8] + m[9]; // pulse 3 sits in samples 8..9 or 9..10
        p2 = (uint32_t)m[9] + m[10];
        if (p2 > p)
            p = p2;
        if (p * 450u <= t)
            goto next;
        psum += p;
        // ... and the four together must clear a higher bar (a sum of four
        // is steadier than each alone): mean pair > (gate2/100) * 2 * (q/9)
        if (psum * 450u <= gate2 * q * 4u)
            goto next;

        // ---- 2. timing: rank the five sub-sample offsets -------------------
        // by the energy in the four pulse chips; the true alignment
        // maximises it.
        for (f = 0; f < 5; f++)
        {
            uint32_t h = 0;
            for (k = 0; k < 4; k++)
                h += chip_sum(m + t_pi[f][k], 0, t_pf[f][k]);
            H[f] = h;
            order[f] = f;
        }
        for (k = 1; k < 5; k++) // sort offsets by pulse energy, best first
        {
            int o = order[k];
            a = k;
            while (a > 0 && H[order[a - 1]] < H[o])
            {
                order[a] = order[a - 1];
                a--;
            }
            order[a] = o;
        }
        // Coarse filter: mean pulse-chip level (H/24 per sample) against the
        // always-quiet level (q/9):  H * 75 >= PREFILTER * q * 2
        if (H[order[0]] * 75u < PREFILTER_X100 * q * 2u)
            goto next;

        // ---- 3. slice and decode the best offsets ---------------------------
        mm.crcok = 0;
        for (a = 0; a < tries && a < 5; a++)
        {
            uint32_t l = 0;
            f = order[a];
            // Full preamble test at this alignment: the mean of the four
            // pulse chips must be at least pre/100 times the mean of the
            // twelve quiet chips:  (H/4) >= (pre/100) * (L/12)
            for (k = 0; k < 12; k++)
                l += chip_sum(m + t_qi[f][k], 0, t_qf[f][k]);
            if (H[f] * 300u < pre * l)
                continue;
            delta = slice_frame(m, (uint32_t)f, msg, conf, &nbits);
            signal = H[f] / 24u;
            self->last_delta = delta;
            self->last_frame_offset = n;
            self->last_frame_phase = (uint8_t)f;
            mode_s_decode(self, &mm, msg);
            if (!attempted)
            {
                memcpy(first_msg, msg, sizeof(first_msg));
                memcpy(first_conf, conf, sizeof(first_conf));
                first_delta = delta;
                first_bits = nbits;
                first_f = f;
            }
            attempted = 1;
            if (mm.crcok)
                break;
        }
        if (!attempted)
            goto next;

        // ---- 4. soft rescue --------------------------------------------------
        if (!mm.crcok && self->soft_fix && first_bits == LONG_BITS &&
            ((first_msg[0] >> 3) == 17 || (first_msg[0] >> 3) == 18))
        {
            unsigned char fixed[MODE_S_LONG_MSG_BYTES];
            int flips;
            memcpy(fixed, first_msg, sizeof(fixed));
            flips = soft_rescue(fixed, first_conf, self->soft_fix);
            if (flips)
            {
                uint32_t addr = ((uint32_t)fixed[1] << 16) | ((uint32_t)fixed[2] << 8) | fixed[3];
                int df_now = fixed[0] >> 3;
                // Same safeguards as exact 2-bit repair: the type must still
                // be an extended squitter and, when asked, the aircraft
                // must already have been heard error-free.
                if ((df_now == 17 || df_now == 18) &&
                    !(self->fix2_known_icao && !icao_addr_was_recently_seen(self, addr)))
                {
                    self->last_delta = first_delta;
                    self->last_frame_phase = (uint8_t)first_f;
                    mode_s_decode(self, &mm, fixed);
                    if (mm.crcok)
                    {
                        mm.errorbit = flips << 16; // marks a soft repair of 'flips' bits
                        delta = first_delta;
                        self->stat_soft_fix++;
                    }
                }
            }
        }

        // ---- accounting and delivery (mirrors mode_s_detect) -----------------
        self->stat_preambles++;
        if (mm.crcok)
        {
            self->stat_crc_ok++;
            self->stat_signal_sum += signal;
            self->stat_delta_sum += delta;
        }
        else
        {
            self->stat_crc_fail++;
            self->stat_fail_delta_sum += first_delta;
        }
        if (self->check_crc == 0 || mm.crcok)
        {
            if (mm.msgtype == 18 && mm.crcok)
            {
                self->stat_df18++;
                self->stat_df18_cf[mm.msg[0] & 7]++;
            }
            if (!(mm.msgtype == 18 && !self->pass_df18))
                cb(self, &mm);
        }
        if (mm.crcok)
        {
            // jump over the frame just decoded: (8 + bits) us * 2.4 samples
            n += ((8u + (uint32_t)mm.msgbits) * U_PER_US) / 5u;
            if (n < limit)
                q = quiet9(mag + n);
            continue;
        }

    next:
        // slide the quiet sum one sample: drop m[5] and m[13], add m[8] and m[19]
        q += (uint32_t)m[8] + m[19];
        q -= (uint32_t)m[5] + m[13];
        n++;
    }

    self->detect_resume = (n > limit) ? (n - limit) : 0;
    self->detect_start = 0;
}
