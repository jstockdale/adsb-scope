// gain_ctl.h - adaptive tuner gain from signal levels and yield.
//
// Used with the 2.4 Msps demodulator. The older loop in class_driver.c
// steers on "CRC error rate" - failed candidates over all candidates -
// which the 2.0 Msps demodulator makes meaningful and the 2.4 Msps one does
// not: its preamble thresholds are ratios, so the number of candidates that
// fail is the same at every gain, and the figure sits near 99 % whatever
// the receiver is doing (docs/DECODER.md section 5.8 has the measurements).
//
// What does change with gain, and is measured here:
//
//   headroom  how far ADC full scale is above the noise, in dB. Too little
//             gain and the noise falls under one ADC step: weak frames are
//             lost to the ADC's own steps. That is the cold end.
//   loud      share of decoded frames whose pulses are within 3 dB of full
//             scale. Frames that clip are the ones too much gain costs, so
//             this is the hot end. (Noise that reaches the rails by itself,
//             headroom under about 20 dB, is the same end, further on.)
//   yield     positions decoded per second. The final word: a step that
//             costs more than 15 % of it is taken back.
//
// Phase 1 (10 s windows) puts the gain where neither end complains. Phase
// 2 (60 s windows) keeps it there and explores one step at a time: up
// while nothing is loud and headroom remains, down while too much is loud;
// each step is confirmed or taken back by the yield, with the ceiling /
// floor / profile logic of the older loop.
//
// Pure C with no ESP-IDF in it: harness/gain/ runs this file on a PC
// against a model of a receiver.

#ifndef GAIN_CTL_H
#define GAIN_CTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAIN_CTL_MAX_STEPS 32

// ---- thresholds (dB of headroom, shares 0..1) --------------------------
// From a sweep of ADC gain over a recorded band (harness/experiments/
// gain_sweep.py, results/gain_sweep.txt): the messages decoded are within
// 3.5 % of the best from 43 down to 30 dB of headroom, and what is lost
// beyond that is lost with the loud share: 7-20 % loud at the best, 3 %
// lost at 30 %, 7 % at 39 %, 12 % at 48 %, a quarter at 57 %.
#define GAIN_HEAD_COLD      44.0f // phase 1: more headroom than this = noise under 0.8 count: raise
#define GAIN_HEAD_UP        33.0f // phase 1 raises only while more than this would remain ...
#define GAIN_HEAD_KEEP      30.0f // ... and no step up may leave less than this
#define GAIN_HEAD_MIN       20.0f // less than this: the noise alone is close to the rails
#define GAIN_HEAD_AIM       26.0f // where a step down from under GAIN_HEAD_MIN aims
#define GAIN_LOUD_HOT       0.35f // phase 1: step down 3 dB at this loud share ...
#define GAIN_LOUD_VERY_HOT  0.50f // ... and 6 dB at this one; in phase 2: back to phase 1
#define GAIN_LOUD_HIGH      0.20f // phase 2: explore one step down above this
#define GAIN_LOUD_LOW       0.05f // steps up only while the loud share is under this
#define GAIN_LOUD_MIN_OK    30u   // decoded frames a window needs for its loud share to count

#define GAIN_P1_WINDOW_US   10000000u // phase 1 window ...
#define GAIN_P1_MIN_US       2000000u // ... or this long, once GAIN_P1_MSGS frames are decoded
#define GAIN_P1_MSGS             200u
#define GAIN_P1_STABLE             3  // windows without a change before phase 2
#define GAIN_P2_WINDOW_US   60000000u
#define GAIN_SETTLE_US        500000u // after a change: samples to let pass before the next window

// What one window saw. All counts are for the window.
typedef struct
{
    float secs;          // length of the window
    uint32_t ok;         // frames decoded (any format, after all repairs)
    uint32_t pos;        // positions decoded
    uint32_t loud;       // of ok: pulses within 3 dB of ADC full scale
    uint32_t es_ok;      // extended squitters decoded
    uint32_t es_lost;    // ... that were clearly there and could not be read
    uint32_t avg_signal; // mean pulse level of the decoded frames (360 per ADC count)
    uint32_t avg_delta;  // mean bit delta of the decoded frames
    float noise;         // rms noise per rail, ADC counts; 0 = not measured
    float clip;          // share of samples on a rail
    float range_nm;      // farthest aircraft; only printed
} gain_obs_t;

typedef struct
{
    // The tuner's gain steps in tenths of a dB, ascending, and the lowest
    // index the loop may use.
    const uint16_t *tab;
    int count;
    int floor_idx;

    int idx;    // gain step in use
    int phase;  // 1 = placing, 2 = steady
    int stable; // phase 1: windows without a change

    // Phase 2
    bool ema_valid;
    float pos_ema;  // positions per second
    float err_ema;  // squitters lost / squitters seen
    float loud_ema; // loud share
    float head_ema; // headroom, dB (0 = no measurement yet)
    float pre_pos;  // pos_ema before the step being judged
    uint32_t pre_sig;
    int prev_idx;   // where that step came from
    int step_dir;   // +1 / -1 while a step is being judged
    int cooldown;   // windows to wait before judging
    int ceiling, ceiling_strikes, ceiling_age, ceiling_calm;
    int floor, floor_strikes, floor_age, floor_calm;
    float profile[GAIN_CTL_MAX_STEPS];
    uint16_t profile_n[GAIN_CTL_MAX_STEPS];

    // Last window, for status displays
    float err_last;   // squitters lost / seen, 0..1 (phase 2: the average)
    float pos_last;   // positions per second (phase 2: the average)
    float head_last;  // dB, 0 = not measured
    float noise_last; // counts
    float loud_last;  // 0..1, -1 = too few frames to say
    float clip_last;  // 0..1
    uint32_t sig_last, delta_last;
} gain_ctl_t;

// tab must stay valid. The loop starts in phase 1 at the highest step.
void gain_ctl_init(gain_ctl_t *g, const uint16_t *tab_tenths, int count, int floor_idx);

// Phase 1 again: from the highest step (from_max), or from where it is.
// The profile is forgotten when starting from the top.
void gain_ctl_restart(gain_ctl_t *g, bool from_max);

// Length of the window the loop wants next, and how few decoded frames end
// a phase 1 window early (0 = never early).
uint32_t gain_ctl_window_us(const gain_ctl_t *g);

// One window. Returns the gain index to use from now on (g->idx; the caller
// applies it when it differs from before). Up to two lines of log text are
// written to 'log', each ended by '\n', without a time stamp; "" if there is
// nothing to say.
int gain_ctl_window(gain_ctl_t *g, const gain_obs_t *o, char *log, size_t log_size);

// Squitters lost over squitters seen, or -1 when the window saw fewer than
// ten: the error rate shown to the user. It is NOT what the loop steers on.
float gain_ctl_err(const gain_obs_t *o);

// Headroom in dB for a noise figure in counts (0 for 0).
float gain_ctl_headroom_db(float noise);

#ifdef __cplusplus
}
#endif

#endif // GAIN_CTL_H
