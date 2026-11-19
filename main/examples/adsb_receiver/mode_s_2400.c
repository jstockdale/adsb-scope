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
//   5. Optional coherent second look (mode_s_coh.c): a frame that still
//      fails is re-sliced from IQ on its own carrier, which survives an
//      overlapping transmission far better than magnitudes do.
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
#ifndef COH_DEDUP
#define COH_DEDUP 4u                // a second look within this many ticks of the last one is the same frame
#endif
#define ES_LOST_HOLD 288u           // samples a long frame lasts (120 us): one lost squitter is counted once
#define ES_LOST_SAME 3u             // a decode this close after a "lost" one is the same frame

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

// The chip that starts u fifths of a sample after m[0], for a u the compiler
// knows: the sample index and the two weights fold to constants. PULSES_AT(f)
// and QUIET_AT(f) are the four pulse chips and the twelve quiet chips of a
// preamble that starts f fifths in - the same positions PULSE_U and QUIET_U
// list above, which the tables t_pi.. hold for the plain form.
#define CHIP_AT(u) ((uint32_t)(5 - (u) % 5) * m[(u) / 5] + (uint32_t)(1 + (u) % 5) * m[(u) / 5 + 1])
#define PULSES_AT(f) (CHIP_AT((f) + 0) + CHIP_AT((f) + 12) + CHIP_AT((f) + 42) + CHIP_AT((f) + 54))
#define QUIET_AT(f)                                                                                  \
    (CHIP_AT((f) + 6) + CHIP_AT((f) + 18) + CHIP_AT((f) + 24) + CHIP_AT((f) + 30) + CHIP_AT((f) + 36) + \
     CHIP_AT((f) + 48) + CHIP_AT((f) + 60) + CHIP_AT((f) + 66) + CHIP_AT((f) + 72) + CHIP_AT((f) + 78) + \
     CHIP_AT((f) + 84) + CHIP_AT((f) + 90))

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
        int32_t d;
        uint32_t ad;
#ifdef MODE_S_2400_PLAIN
        // As the model reads: the bit's first chip minus its second.
        uint32_t ib = i + 1, fb = f + 1; // second chip: +6 U
        if (fb >= 5)
        {
            fb -= 5;
            ib++;
        }
        d = (int32_t)chip_sum(m, i, f) - (int32_t)chip_sum(m, ib, fb);
#else
        // The same difference with the two chip sums written out. For a
        // chip at fraction f the first is (5-f) m[i] + (1+f) m[i+1] and the
        // second, one sample and one fifth on, (4-f) m[i+1] + (2+f) m[i+2]
        // (at f = 4 it starts on a sample: 5 m[i+2] + m[i+3]), so the bit is
        // a three- or four-tap filter whose weights depend on f alone.
        // Half the work of two chip sums; harness/check_stream.py holds the
        // two forms to identical output.
        const int32_t m0 = m[i], m1 = m[i + 1], m2 = m[i + 2];
        switch (f)
        {
        case 0:
            d = 5 * m0 - 3 * m1 - 2 * m2;
            break;
        case 1:
            d = 4 * m0 - m1 - 3 * m2;
            break;
        case 2:
            d = 3 * m0 + m1 - 4 * m2;
            break;
        case 3:
            d = 2 * m0 + 3 * m1 - 5 * m2;
            break;
        default:
            d = m0 + 5 * m1 - 5 * m2 - (int32_t)m[i + 3];
            break;
        }
#endif
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
    uint32_t acc = 0;
    int n = 0, k, j, g, best_mask = 0, best_flips = SOFT_MAX_FLIPS + 1, mask = 0, flips = 0;

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

    // Every non-empty subset of those bits, in Gray-code order: each step
    // changes one bit, so the subset's syndrome is one XOR from the last.
    // Chosen as before: the fewest flips, and among equals the smallest mask.
    for (g = 1; g < (1 << n); g++)
    {
        int bit = 0;
        while (!((g >> bit) & 1))
            bit++;
        mask ^= 1 << bit;
        acc ^= mode_s_syn_bit_112[idx[bit]];
        flips += ((mask >> bit) & 1) ? 1 : -1;
        if (acc == syn && (flips < best_flips || (flips == best_flips && mask < best_mask)))
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

// Coherent second look at the candidate at mag[n] + f/5 that failed its CRC.
// Gates first (they decide the cost), then the re-slice, then the same
// checks every repaired frame gets. Returns 1 with *mm decoded.
// Kept out of line: it runs for a small share of candidates, and inlined it
// costs the detection loop registers on every sample.
static __attribute__((noinline)) int coh_second_look(mode_s_t *self, uint32_t n, int f, const unsigned char *first_msg,
                           uint32_t first_delta, struct mode_s_msg *mm)
{
    unsigned char cm[MODE_S_LONG_MSG_BYTES];
    uint32_t cconf[LONG_BITS];
    uint64_t tick = (self->stream_base + n) * 5u + (uint32_t)f + 1u;
    int dt = 0, flips = 0, df, r;
    uint32_t t;
    float fhz = 0;

    if (self->coh_df_gate)
    {
        // The first slice's format field: 17 or 18 exactly (gate 1), or within
        // one bit of either (gate 2). Other Mode S replies and most noise
        // fail this; an extended squitter with a damaged first byte may too.
        unsigned a = (unsigned)(first_msg[0] >> 3) ^ 17u, b = (unsigned)(first_msg[0] >> 3) ^ 18u;
        if (self->coh_df_gate == 1 ? (a && b) : ((a & (a - 1u)) && (b & (b - 1u))))
            return 0;
    }
    // The bit delta of a pure-noise candidate sits close to the running
    // level of all failed candidates; a frame worth a second look stands
    // well above it.
    if (self->coh_gate_x100 &&
        (uint64_t)first_delta * 1600u < (uint64_t)self->coh_gate_x100 * self->coh_ref)
        return 0;
    // One attempt per frame: the candidates one sample either side of it
    // are the same frame, and the timing search has already covered them.
    if (self->coh_last_tick && tick + COH_DEDUP >= self->coh_last_tick && tick <= self->coh_last_tick + COH_DEDUP)
        return 0;
    self->stat_coh_gated++;
    r = mode_s_coh_reslice(self, n, f, cm, cconf, &dt, &fhz);
    if (r == 0)
        return 0;
    self->coh_last_tick = tick;
    self->stat_coh_try++;
    if (r < 0)
    {
        self->stat_coh_abort++;
        return 0;
    }

    df = cm[0] >> 3;
    if (df != 17 && df != 18)
        return 0;
    if (mode_s_crc_syndrome(cm, LONG_BITS) != 0)
    {
        uint32_t addr;
        if (!self->soft_fix)
            return 0;
        flips = soft_rescue(cm, cconf, self->soft_fix);
        if (!flips)
            return 0;
        df = cm[0] >> 3;
        addr = ((uint32_t)cm[1] << 16) | ((uint32_t)cm[2] << 8) | cm[3];
        if ((df != 17 && df != 18) ||
            (self->fix2_known_icao && !icao_addr_was_recently_seen(self, addr)))
            return 0;
    }
    // The frame's time as re-sliced: the reported timing plus the correction.
    t = 5u * n + (uint32_t)f + (uint32_t)dt; // dt >= -2 and 5n + f >= 2 here in practice
    if (dt < 0 && 5u * n + (uint32_t)f < (uint32_t)(-dt))
        t = 0;
    self->last_frame_offset = t / 5u;
    self->last_frame_phase = (uint8_t)(t % 5u);
    self->last_delta = first_delta;
    mode_s_decode(self, mm, cm); // the frame is CRC-clean here, so this only reads its fields
    if (!mm->crcok)
        return 0;
    self->last_freq_hz = fhz;
    self->stat_coh_ok++;
    if (flips)
    {
        mm->errorbit = flips << 16;
        self->stat_coh_soft++;
    }
    return 1;
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
        uint32_t delta = 0, first_delta = 0, signal = 0, ref_before;
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
#ifdef MODE_S_2400_PLAIN
        for (f = 0; f < 5; f++)
        {
            uint32_t h = 0;
            for (k = 0; k < 4; k++)
                h += chip_sum(m + t_pi[f][k], 0, t_pf[f][k]);
            H[f] = h;
            order[f] = f;
        }
#else
        // The same sums with every chip position worked out by the
        // compiler (PULSES_AT): no table to read, no loop.
        H[0] = PULSES_AT(0);
        H[1] = PULSES_AT(1);
        H[2] = PULSES_AT(2);
        H[3] = PULSES_AT(3);
        H[4] = PULSES_AT(4);
        for (f = 0; f < 5; f++)
            order[f] = f;
#endif
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
#ifdef MODE_S_2400_PLAIN
            for (k = 0; k < 12; k++)
                l += chip_sum(m + t_qi[f][k], 0, t_qf[f][k]);
#else
            switch (f)
            {
            case 0:
                l = QUIET_AT(0);
                break;
            case 1:
                l = QUIET_AT(1);
                break;
            case 2:
                l = QUIET_AT(2);
                break;
            case 3:
                l = QUIET_AT(3);
                break;
            default:
                l = QUIET_AT(4);
                break;
            }
#endif
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

        // ---- 5. coherent second look -----------------------------------------
        if (!mm.crcok && self->coh_fix && self->iq_cur)
        {
            if (coh_second_look(self, n, first_f, first_msg, first_delta, &mm))
                delta = first_delta;
        }
        ref_before = self->coh_ref;
        if (!mm.crcok)
        {
            // Noise reference: the lower quartile of the bit delta of failed
            // candidates (x16), tracked by stepping up a little when a
            // candidate is above it and three times as far down when below.
            // Most failed candidates are noise, and those that are real
            // signals sit above the quartile, so traffic does not drag it up.
            // (Kept whether or not the coherent look is on: the squitter
            // accounting below uses it too. Nothing else reads it.)
            uint32_t d16 = first_delta << 4;
            if (!self->coh_ref)
                self->coh_ref = d16;
            else
            {
                uint32_t step = (self->coh_ref >> 8) + 1u;
                if (d16 > self->coh_ref)
                    self->coh_ref += step;
                else if (self->coh_ref > 3u * step)
                    self->coh_ref -= 3u * step;
            }
        }

        // ---- accounting and delivery (mirrors mode_s_detect) -----------------
        // stat_crc_fail counts EVERY candidate that did not decode. At this
        // demodulator's thresholds most of them are not Mode S frames at all
        // (Mode A/C replies, pulse noise), and their number does not depend
        // on the gain: stat_crc_fail / stat_preambles is close to 1 whatever
        // the receiver is doing, and is NOT an error rate. The figures meant
        // for that are stat_es_ok and stat_es_lost.
        self->stat_preambles++;
        if (mm.crcok)
        {
            self->stat_crc_ok++;
            self->stat_signal_sum += signal;
            self->stat_delta_sum += delta;
            if (signal >= MODE_S_LOUD_LEVEL)
                self->stat_loud++;
            if (mm.msgtype == 17 || mm.msgtype == 18)
            {
                uint64_t at = self->stream_base + n + 1u;
                self->stat_es_ok++;
                // The same frame, counted as lost a sample or two earlier on
                // a neighbouring alignment, was not lost after all.
                if (self->es_lost_at && at - self->es_lost_at <= ES_LOST_SAME && self->stat_es_lost)
                    self->stat_es_lost--;
                self->es_lost_at = 0;
            }
        }
        else
        {
            self->stat_crc_fail++;
            self->stat_fail_delta_sum += first_delta;
            // A squitter that was there and could not be read: the slice
            // says DF17 or DF18, and its bits stand at least twice as far
            // apart as those of the run of failed candidates (the level the
            // coherent look asks for too). Counted once per frame: whatever
            // else fails inside the 120 us it lasts is the same frame.
            if (first_bits == LONG_BITS && ref_before && (uint64_t)first_delta * 8u >= ref_before &&
                ((first_msg[0] >> 3) == 17 || (first_msg[0] >> 3) == 18))
            {
                uint64_t at = self->stream_base + n + 1u;
                if (!self->es_lost_at || at - self->es_lost_at >= ES_LOST_HOLD)
                {
                    self->stat_es_lost++;
                    self->es_lost_at = at;
                }
            }
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
