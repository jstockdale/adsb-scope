// mode_s_coh.c - a coherent second look at a frame that failed its CRC
// (2.4 Msps demodulator only).
//
// Why. mode_s_2400.c slices on magnitudes. When another transmission
// overlaps the frame, a pulse of the other signal in the "empty" half of a
// bit beats the frame's own pulse unless the frame is roughly 6 dB or more
// stronger - worse when the two add out of phase. Seen in IQ, the frame's
// pulses all sit on one carrier: project every chip onto that carrier and
// most of the other signal's contribution drops out of the comparison of
// the two halves of a bit. No change to detection; this only re-slices a
// frame the detector already found and could not decode.
//
// Method, for a frame whose preamble starts at sample n + f/5:
//   1. Matched-filter the 224 data chips from IQ, with the same two-sample
//      weights the magnitude slicer uses (integer arithmetic).
//   2. Take the stronger chip of each bit as "on". Products of neighbouring
//      on-chips (0.5 and 1.0 us apart) give a coarse carrier frequency.
//   3. Remove it, sum the on-chips in 4 us blocks and take a 64-point FFT:
//      the peak, interpolated, refines the frequency to a few hundred Hz.
//      If the peak holds less than COH_MIN_COHERENCE of the on-chip energy
//      there is no single carrier to project onto: stop.
//   4. For the reported timing and its neighbours (+-coh_tsearch fifths of
//      a sample): rotate the chips to baseband, estimate the phase from the
//      on-chips, and slice each bit on the in-phase difference of its two
//      chips. Keep the timing with the widest eye.
// The caller checks the CRC and applies the soft rescue with the new
// per-bit confidences (mode_s_2400.c).
//
// Cost (harness/results/coh_cost.txt; x86-64 instruction counts, not P4
// cycles): about 38,000 instructions per attempt on real traffic, where
// half the attempts stop at step 3, and 48,000-51,000 in the densest
// simulated traffic. With the default gates that is 5-6 % more decoder
// instructions on the Madrid capture, 13 % at 2,000 messages/s and
// nothing on a quiet band.
// Working memory: mode_s_coh_work_size() bytes (9.6 kB), supplied by the
// application (PSRAM on the device). No heap, no static RAM, no doubles.
//
// Written for this project; it shares no code with other decoders.

#include <math.h>
#include <string.h>
#include "mode-s.h"

#define NCHIP 224
#define NBIT 112
#define NFFT 64
#define PREAMBLE_TICKS 96
#define LOCAL_SAMPLES 304
#define TWO_PI_F 6.28318530718f
#define COH_MIN_COHERENCE 0.30f // least share of on-chip energy in the carrier peak

struct mode_s_coh_work
{
    // IQ of the magnitude samples carried between chunks, so a frame that
    // starts in the carried tail can still be re-sliced.
    unsigned char iq_tail[2 * MODE_S_STREAM_TAIL_MAX];
    // Scratch for one attempt
    int16_t xi[LOCAL_SAMPLES], xq[LOCAL_SAMPLES];
    int16_t c0r[NCHIP], c0i[NCHIP], cr[NCHIP], ci[NCHIP];
    float rot_re[NCHIP], rot_im[NCHIP];
    float dr[NCHIP], di[NCHIP];
    float zr[NBIT], zi[NBIT];
    float d[NBIT], best_d[NBIT];
    float br[NFFT], bi[NFFT];
    uint8_t tent[NBIT];
};

// 64-point FFT tables: cos and sin of -2 pi i / 64, and the bit-reversed
// index. Constants, so they sit in flash and host and device use the same
// values.
static const float tw_re[NFFT / 2] = {
    1.0f, 0.99518472f, 0.980785251f, 0.956940353f,
    0.923879504f, 0.881921232f, 0.831469595f, 0.773010433f,
    0.707106769f, 0.634393275f, 0.555570185f, 0.471396655f,
    0.382683426f, 0.290284634f, 0.195090234f, 0.0980171338f,
    -4.37113883e-08f, -0.0980172232f, -0.195090324f, -0.290284723f,
    -0.382683516f, -0.471396834f, -0.555570364f, -0.634393275f,
    -0.707106769f, -0.773010492f, -0.831469655f, -0.881921351f,
    -0.923879623f, -0.956940353f, -0.98078531f, -0.99518472f,
};
static const float tw_im[NFFT / 2] = {
    -0.0f, -0.0980171412f, -0.195090324f, -0.290284693f,
    -0.382683456f, -0.471396744f, -0.555570245f, -0.634393334f,
    -0.707106769f, -0.773010433f, -0.831469655f, -0.881921291f,
    -0.923879504f, -0.956940353f, -0.98078531f, -0.99518472f,
    -1.0f, -0.99518472f, -0.980785251f, -0.956940293f,
    -0.923879504f, -0.881921232f, -0.831469536f, -0.773010492f,
    -0.707106769f, -0.634393275f, -0.555570185f, -0.471396625f,
    -0.382683277f, -0.290284723f, -0.195090309f, -0.0980170965f,
};
static const uint8_t bitrev[NFFT] = {
    0, 32, 16, 48, 8, 40, 24, 56, 4, 36, 20, 52, 12, 44, 28, 60,
    2, 34, 18, 50, 10, 42, 26, 58, 6, 38, 22, 54, 14, 46, 30, 62,
    1, 33, 17, 49, 9, 41, 25, 57, 5, 37, 21, 53, 13, 45, 29, 61,
    3, 35, 19, 51, 11, 43, 27, 59, 7, 39, 23, 55, 15, 47, 31, 63,
};

static void fft64(float *re, float *im)
{
    int i, k, len;
    for (i = 0; i < NFFT; i++)
    {
        int j = bitrev[i];
        if (j > i)
        {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (len = 2; len <= NFFT; len <<= 1)
    {
        int half = len >> 1, step = NFFT / len;
        for (i = 0; i < NFFT; i += len)
        {
            for (k = 0; k < half; k++)
            {
                float wr = tw_re[k * step], wi = tw_im[k * step];
                float xr = re[i + k + half] * wr - im[i + k + half] * wi;
                float xi = re[i + k + half] * wi + im[i + k + half] * wr;
                re[i + k + half] = re[i + k] - xr;
                im[i + k + half] = im[i + k] - xi;
                re[i + k] += xr;
                im[i + k] += xi;
            }
        }
    }
}

size_t mode_s_coh_work_size(void)
{
    return sizeof(struct mode_s_coh_work);
}

// Called by the streaming front end around each detect call (mode-s.c).
void mode_s_coh_stream_begin(mode_s_t *self, const unsigned char *iq, uint32_t n, uint32_t tail)
{
    self->iq_cur = iq;
    self->iq_cur_n = n;
    self->iq_tail_n = tail;
}

void mode_s_coh_stream_end(mode_s_t *self, const unsigned char *iq, uint32_t n, uint32_t tail,
                           uint32_t keep_from, uint32_t total)
{
    struct mode_s_coh_work *w = (struct mode_s_coh_work *)self->coh_work;
    uint32_t keep = total - keep_from; // magnitude samples carried to the next call
    self->iq_cur = NULL;
    if (!w || keep > MODE_S_STREAM_TAIL_MAX)
    {
        self->iq_tail_valid = 0;
        return;
    }
    if (keep_from >= tail)
    {
        memcpy(w->iq_tail, iq + 2u * (keep_from - tail), 2u * keep);
        self->iq_tail_valid = keep;
    }
    else if (self->iq_tail_valid == tail)
    {
        uint32_t from_old = tail - keep_from;
        memmove(w->iq_tail, w->iq_tail + 2u * keep_from, 2u * from_old);
        memcpy(w->iq_tail + 2u * from_old, iq, 2u * n);
        self->iq_tail_valid = keep;
    }
    else
    {
        self->iq_tail_valid = 0;
    }
}

// IQ of magnitude samples [j0, j0 + count) of the current detect call.
static int fetch_iq(const mode_s_t *self, const struct mode_s_coh_work *w,
                    uint32_t j0, uint32_t count, int16_t *xi, int16_t *xq)
{
    uint32_t tail = self->iq_tail_n, k;
    if (!self->iq_cur || j0 + count > tail + self->iq_cur_n)
        return 0;
    if (j0 < tail && self->iq_tail_valid != tail)
        return 0;
    for (k = 0; k < count && j0 + k < tail; k++) // the part in the carried tail
    {
        const unsigned char *p = w->iq_tail + 2u * (j0 + k);
        xi[k] = (int16_t)((int)p[0] - 127);
        xq[k] = (int16_t)((int)p[1] - 127);
    }
    for (; k < count; k++)
    {
        const unsigned char *p = self->iq_cur + 2u * (j0 + k - tail);
        xi[k] = (int16_t)((int)p[0] - 127);
        xq[k] = (int16_t)((int)p[1] - 127);
    }
    return 1;
}

// Complex matched-filter output of the 224 data chips; the first starts u0
// ticks (fifths of a sample) after xi[0]. Same weights as chip_sum(). A chip
// is 6 ticks, so the pattern of (sample, weights) repeats every 5 chips =
// 6 samples; the five cases are worked out once and the loop has no tests.
static void chips_at(const int16_t *xi, const int16_t *xq, uint32_t u0, int16_t *cr, int16_t *ci)
{
    uint32_t off[5];
    int32_t wa[5], wb[5];
    const int16_t *pi_ = xi + u0 / 5, *pq_ = xq + u0 / 5;
    uint32_t f0 = u0 % 5;
    int c, j;
    for (j = 0; j < 5; j++)
    {
        uint32_t u = f0 + 6u * (uint32_t)j;
        off[j] = u / 5;
        wa[j] = (int32_t)(5 - u % 5);
        wb[j] = (int32_t)(1 + u % 5);
    }
    for (c = 0; c + 5 <= NCHIP; c += 5, pi_ += 6, pq_ += 6)
    {
        for (j = 0; j < 5; j++)
        {
            cr[c + j] = (int16_t)(wa[j] * pi_[off[j]] + wb[j] * pi_[off[j] + 1]);
            ci[c + j] = (int16_t)(wa[j] * pq_[off[j]] + wb[j] * pq_[off[j] + 1]);
        }
    }
    for (j = 0; c < NCHIP; c++, j++)
    {
        cr[c] = (int16_t)(wa[j] * pi_[off[j]] + wb[j] * pi_[off[j] + 1]);
        ci[c] = (int16_t)(wa[j] * pq_[off[j]] + wb[j] * pq_[off[j] + 1]);
    }
}

// Rotate the chips to baseband, take the phase from the on-chips and slice
// each bit on the in-phase difference of its two chips. Returns the eye
// opening (sum of |difference|), or -1 if there is no signal.
static float slice_coherent(struct mode_s_coh_work *w, const int16_t *xr, const int16_t *xim)
{
    float accr = 0, acci = 0, mag, ur, ui, q = 0;
    int c, k;
    for (c = 0; c < NCHIP; c++)
    {
        float a = (float)xr[c], b = (float)xim[c];
        w->dr[c] = a * w->rot_re[c] - b * w->rot_im[c];
        w->di[c] = a * w->rot_im[c] + b * w->rot_re[c];
    }
    for (k = 0; k < NBIT; k++)
    {
        c = 2 * k + (w->tent[k] ? 0 : 1);
        accr += w->dr[c];
        acci += w->di[c];
    }
    mag = sqrtf(accr * accr + acci * acci);
    if (mag <= 0.0f)
        return -1.0f;
    ur = accr / mag;
    ui = acci / mag;
    for (k = 0; k < NBIT; k++)
    {
        float a = w->dr[2 * k] * ur + w->di[2 * k] * ui;
        float b = w->dr[2 * k + 1] * ur + w->di[2 * k + 1] * ui;
        w->d[k] = a - b;
        q += fabsf(a - b);
    }
    return q;
}

static void bits_out(const float *d, unsigned char *msg, uint32_t *conf)
{
    int k;
    memset(msg, 0, MODE_S_LONG_MSG_BYTES);
    for (k = 0; k < NBIT; k++)
    {
        float v = d[k];
        if (v > 0.0f)
            msg[k >> 3] |= (unsigned char)(0x80 >> (k & 7));
        conf[k] = (uint32_t)(fabsf(v) * 16.0f);
    }
}

int mode_s_coh_reslice(mode_s_t *self, uint32_t n, int f, unsigned char *msg, uint32_t *conf,
                       int *dt_out, float *freq_hz)
{
    struct mode_s_coh_work *w = (struct mode_s_coh_work *)self->coh_work;
    const int ts = self->coh_tsearch > 2 ? 2 : self->coh_tsearch;
    uint32_t T, s0, last, count;
    float r1r = 0, r1i = 0, r05r = 0, r05i = 0;
    float w1, wf, pr, pi, e1r, e1i, ehr, ehi, sr, si, best_q = -1.0f;
    float pmax = -1.0f, pa, pb, pc, den, kk, energy = 0, q;
    int k, c, dt, kmax = 0, best_dt = 0;

    if (!w)
        return 0;

    T = 5u * n + (uint32_t)f;                                   // preamble start, ticks from mag[0]
    s0 = (T + PREAMBLE_TICKS - (uint32_t)ts) / 5u;              // first sample needed
    last = (T + PREAMBLE_TICKS + (uint32_t)ts + 6u * (NCHIP - 1)) / 5u + 1u;
    count = last - s0 + 1u;
    if (count > LOCAL_SAMPLES || !fetch_iq(self, w, s0, count, w->xi, w->xq))
        return 0;
#define U0(dt) ((uint32_t)((int32_t)(T + PREAMBLE_TICKS - 5u * s0) + (dt)))

    // 1. chips at the reported timing; 2. the stronger chip of each bit
    chips_at(w->xi, w->xq, U0(0), w->c0r, w->c0i);
    for (k = 0; k < NBIT; k++)
    {
        int32_t ar = w->c0r[2 * k], ai = w->c0i[2 * k], br_ = w->c0r[2 * k + 1], bi_ = w->c0i[2 * k + 1];
        int first = (ar * ar + ai * ai) > (br_ * br_ + bi_ * bi_);
        w->tent[k] = (uint8_t)first;
        w->zr[k] = (float)(first ? ar : br_);
        w->zi[k] = (float)(first ? ai : bi_);
        energy += w->zr[k] * w->zr[k] + w->zi[k] * w->zi[k];
    }
    for (k = 0; k + 1 < NBIT; k++)
    {
        float qr = w->zr[k + 1] * w->zr[k] + w->zi[k + 1] * w->zi[k];
        float qi = w->zi[k + 1] * w->zr[k] - w->zr[k + 1] * w->zi[k];
        if (w->tent[k] == w->tent[k + 1]) // on-chips exactly 1.0 us apart
        {
            r1r += qr;
            r1i += qi;
        }
        else if (!w->tent[k]) // second chip, then first chip: 0.5 us apart
        {
            r05r += qr;
            r05i += qi;
        }
    }
    w1 = atan2f(r1i, r1r); // rad per microsecond, ambiguous by 2 pi (1 MHz)
    if (r05r != 0.0f || r05i != 0.0f)
    {
        float wh = 2.0f * atan2f(r05i, r05r);
        w1 += TWO_PI_F * roundf((wh - w1) / TWO_PI_F);
    }

    // 3. remove the coarse carrier, sum on-chips in 4 us blocks, FFT
    memset(w->br, 0, sizeof(w->br));
    memset(w->bi, 0, sizeof(w->bi));
    e1r = cosf(w1);
    e1i = -sinf(w1);
    ehr = cosf(0.5f * w1);
    ehi = -sinf(0.5f * w1);
    pr = 1.0f;
    pi = 0.0f;
    for (k = 0; k < NBIT; k++)
    {
        float vr = w->zr[k] * pr - w->zi[k] * pi;
        float vi = w->zr[k] * pi + w->zi[k] * pr;
        float t;
        if (!w->tent[k]) // the on-chip is the second one: half a microsecond later
        {
            t = vr * ehr - vi * ehi;
            vi = vr * ehi + vi * ehr;
            vr = t;
        }
        w->br[k >> 2] += vr;
        w->bi[k >> 2] += vi;
        t = pr * e1r - pi * e1i;
        pi = pr * e1i + pi * e1r;
        pr = t;
    }
    fft64(w->br, w->bi);
    for (k = 0; k < NFFT; k++)
    {
        float p = w->br[k] * w->br[k] + w->bi[k] * w->bi[k];
        if (p > pmax)
        {
            pmax = p;
            kmax = k;
        }
    }
    // If the on-chips share one carrier, the peak holds nearly all their
    // energy (pmax = 112 x energy for a clean frame). Mode A/C replies and
    // noise have no such carrier: stop here, before the costly part.
    if (pmax < COH_MIN_COHERENCE * (float)NBIT * energy)
        return -1;
    k = (kmax + NFFT - 1) % NFFT;
    pa = sqrtf(w->br[k] * w->br[k] + w->bi[k] * w->bi[k]);
    pb = sqrtf(pmax);
    k = (kmax + 1) % NFFT;
    pc = sqrtf(w->br[k] * w->br[k] + w->bi[k] * w->bi[k]);
    den = pa - 2.0f * pb + pc;
    kk = (float)kmax + (den != 0.0f ? 0.5f * (pa - pc) / den : 0.0f);
    if (kk >= (float)(NFFT / 2))
        kk -= (float)NFFT;
    wf = w1 + TWO_PI_F * kk * (0.25f / (float)NFFT); // block rate: 0.25 MHz
    if (freq_hz)
        *freq_hz = wf * (1.0e6f / TWO_PI_F);

    // 4. rotate to baseband, phase from the on-chips, slice in phase;
    //    try the neighbouring timings and keep the widest eye
    sr = cosf(0.5f * wf);
    si = -sinf(0.5f * wf);
    w->rot_re[0] = 1.0f;
    w->rot_im[0] = 0.0f;
    for (c = 1; c < NCHIP; c++)
    {
        w->rot_re[c] = w->rot_re[c - 1] * sr - w->rot_im[c - 1] * si;
        w->rot_im[c] = w->rot_re[c - 1] * si + w->rot_im[c - 1] * sr;
    }
    q = slice_coherent(w, w->c0r, w->c0i);
    if (q >= 0.0f)
    {
        best_q = q;
        memcpy(w->best_d, w->d, sizeof(w->d));
        bits_out(w->best_d, msg, conf);
        if (mode_s_crc_syndrome(msg, NBIT) == 0) // clean at the reported timing: done
        {
            *dt_out = 0;
            return 1;
        }
    }
    for (dt = -ts; dt <= ts; dt++)
    {
        if (dt == 0)
            continue;
        chips_at(w->xi, w->xq, U0(dt), w->cr, w->ci);
        q = slice_coherent(w, w->cr, w->ci);
        if (q > best_q)
        {
            best_q = q;
            best_dt = dt;
            memcpy(w->best_d, w->d, sizeof(w->d));
        }
    }
#undef U0
    if (best_q < 0.0f)
        return 0;
    if (best_dt != 0)
        bits_out(w->best_d, msg, conf);
    *dt_out = best_dt;
    return 1;
}
