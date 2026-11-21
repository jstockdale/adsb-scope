/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "class_driver.h"
#include "serial_console.h"
#include "sd_config.h"
#include "device_settings.h"
#include "aircraft_db.h"
#include "mqtt_feeder.h"
#include "mqtt_batch.h"    // MQTT_BATCH_SLOTS, checked against ADSB_MAX_AIRCRAFT
#include "rtlsdr_i2c.h"
#include "sample_ring.h"
#include "usb_stream.h"
#include "gain_ctl.h"
#include "adsb_fmt.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

// GPS fix epoch — set in GPS task on first quality fix.
// Trusted time check — true when GPS, NTP, or PPS has synced (not just RTC boot seed).
// Used to gate CSV timestamp format and SD log renames.
extern bool time_manager_is_trusted_fn(void);
// SD I/O mutex — defined in main.cpp, serializes file I/O across tasks
extern bool sd_io_take(uint32_t timeout_ms);
extern void sd_io_give(void);

#define RTLSDR_BUF_LEN        (16384 + 512)
#define CLIENT_NUM_EVENT_MSG  5
#define CPR_MAX_AGE_US        10000000LL   // 10 seconds
#define CPR_CACHE_SIZE        ADSB_MAX_AIRCRAFT
#define CPR_JUMP_FLOOR_NM     10.0         // minimum threshold (handles GPS jitter, CPR quantization)
#define CPR_JUMP_RATE_NM_S    1.0          // ~3600 kt — faster than any transponder-equipped aircraft
#define CPR_SUSPECT_GIVE_UP   5            // consecutive suspect events → reset has_position
#define CPR_CONSENSUS_COUNT   3            // agreeing globals needed to override bad good_pos
#define CPR_CONSENSUS_NM      10.0         // max spread between agreeing candidates
#define AIRCRAFT_TABLE_SIZE   ADSB_MAX_AIRCRAFT
// The MQTT feeder must have a slot for every aircraft the table can hold,
// and the list buffer a row for each (the formatter stops 80 bytes short
// of the end of its buffer).
_Static_assert(MQTT_BATCH_SLOTS >= ADSB_MAX_AIRCRAFT, "MQTT batch smaller than the aircraft table");
_Static_assert(ADSB_LIST_BUF_SIZE - 80 > (ADSB_MAX_AIRCRAFT - 1) * ADSB_LIST_ROW_LEN, "aircraft list buffer too small");
#define AIRCRAFT_MAX_AGE_US   60000000LL   // 60 seconds

#define ADSB_READER_TASK_STACK  (8 * 1024)
// The reader only moves finished USB transfers into the sample ring, and
// has about 2 ms to do it before the endpoint runs dry (usb_stream.h). It
// therefore runs above every application task – the decoder (3), MQTT (5),
// audio (6) – and just below the USB host library task that hands it the
// transfers (HOST_LIB_TASK_PRIORITY in main.cpp). Each wake-up is a 5.6 kB
// copy, so the tasks below it lose very little.
#define ADSB_READER_TASK_PRIO   11

// ============================================================
// ADSB reader task signaling
// ============================================================

static TaskHandle_t s_adsb_reader_task_hdl = NULL;
static volatile bool s_adsb_reader_stop = false;

// When true, class_driver_task must NOT call usb_host_client_handle_events —
// the reader task becomes the sole USB event consumer to avoid concurrent
// event pumping on the same client handle (which corrupts the heap).
static volatile bool s_reader_owns_events = false;

// Passed from event callback to the reader task
static volatile uint8_t s_new_dev_addr = 0;

// Pre-allocated demodulation magnitude buffer (allocated once in reader task)
static uint16_t *s_mag_buf = NULL;

// Demodulator and sample rate (menuconfig: Example Configuration ->
// ADS-B demodulator). Both demodulators are always compiled; this picks one.
#ifdef CONFIG_ADSB_DEMOD_2000
#define ADSB_SAMPLE_RATE 2000000u
#define ADSB_DEMOD_IS_2400 0
#else
#define ADSB_SAMPLE_RATE 2400000u
#define ADSB_DEMOD_IS_2400 1
#endif

// DF18 counters, mirrored from the decoder once per chunk by df18_tick()
// so adsb_get_stats() can read them without touching the decoder state.
static volatile uint32_t s_df18_total = 0, s_df18_last_min = 0;
static volatile uint32_t s_df18_tisb = 0, s_df18_adsr = 0, s_df18_adsb_nt = 0, s_df18_other = 0;
static uint32_t s_df18_win_base = 0;
static int64_t  s_df18_win_start = 0;

// Decode load: microseconds spent inside demodulate() in the current
// window, and the result of the last complete window in thousandths of
// one core.
static uint32_t s_decode_busy_us = 0;
static int64_t  s_decode_win_start = 0;
static volatile uint32_t s_decode_load_permille = 0;

// Time spent in the SD log inside the decoder, same windows.
static uint32_t s_sdlog_busy_us = 0;
static volatile uint32_t s_sdlog_load_permille = 0;
static volatile uint32_t s_peak_decode_permille = 0;
// Sample bytes handed to the decoder in the window, for adsb_stats_t.capture
static uint32_t s_capture_bytes_win = 0;
static volatile uint32_t s_capture_permille = 0, s_min_capture_permille = 1000;
static int      s_load_line_windows = 0;

// ---- Sample path (r9): USB reader task -> ring in PSRAM -> decoder task ----
// 192 transfers of 5632 bytes: 1.08 MB, 225 ms of signal at 2.4 Msps.
#define ADSB_RING_SLOTS         192
#define ADSB_DECODE_TASK_STACK  (12 * 1024)
#define ADSB_DECODE_TASK_PRIO   3
// The decoder is the one task here that can use a whole core, and it has
// the second one: core 0 carries the USB host, the timer and network tasks
// and every interrupt handler registered at start-up. Left unpinned (r9,
// r10) it settled on core 0 on the first device test and kept that core's
// idle task from running for as long as the log goes, with core 1 idle.
#define ADSB_DECODE_TASK_CORE   1
// The decoder is woken when this many transfers are waiting (9 ms of
// signal), so it works in a few long runs instead of many short ones.
#define ADSB_DECODE_BATCH_SLOTS 8

static sample_ring_t     s_ring;
static void             *s_ring_mem = NULL;       // PSRAM; allocated once and kept
static TaskHandle_t      s_decode_task_hdl = NULL;
static volatile bool     s_decode_stop = false;
static SemaphoreHandle_t s_decode_done = NULL;
static int               s_usb_xfers = 0;         // USB transfers in use
static int64_t           s_usb_prev_cb_us = 0;    // reader task only
static uint64_t          s_usb_bytes_total = 0;   // reader task only
static volatile uint32_t s_usb_capture_permille = 0;
static volatile uint32_t s_usb_starved = 0;       // times the endpoint ran dry
static volatile uint32_t s_decode_gaps = 0;       // discontinuities the decoder was told about
static volatile int      s_bias_pending = -1;     // -1 none, 0 off, 1 on: applied by the reader task

// ---- Log line queue (r9) ----
// The decoder used to print one line per message itself and so ran at the
// speed of the console (docs/UPLINK.md 2.2). Lines now go into this queue
// and a low-priority task writes them. When the console cannot keep up the
// queue fills and lines are dropped and counted; the decoder never waits.
#define ADSB_LOGQ_BYTES      16384   // power of two; about 1.4 s of console output at 115200 baud, 0.18 s at 921600
#define ADSB_LOG_LINE_MAX    400
#define ADSB_LOG_TASK_STACK  (6 * 1024)
#define ADSB_LOG_TASK_PRIO   2

static char        *s_logq = NULL;                // PSRAM
static atomic_uint  s_logq_head, s_logq_tail;     // free-running byte counts; producer / consumer
static TaskHandle_t s_log_task_hdl = NULL;
static volatile uint32_t s_log_lines_total = 0, s_log_bytes_total = 0, s_log_dropped_total = 0;
static uint32_t s_win_log_lines0 = 0, s_win_log_bytes0 = 0, s_win_log_dropped0 = 0;
static volatile uint32_t s_log_lines_s = 0, s_log_bytes_s = 0, s_log_dropped_win = 0;

static void adsb_logq_push(const char *line, int len);
static bool adsb_logq_has_room(uint32_t len);
static void adsb_logq_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- Observation mark (adsb_stats_mark) ----
static volatile int64_t  s_mark_us = 0;
static volatile bool     s_window_restart = false;  // the decoder starts a fresh 5 s window
static volatile uint32_t s_mark_ring_dropped = 0, s_mark_usb_starved = 0, s_mark_log_dropped = 0;

// ---- Replay (r10) – see adsb_replay_status_t in class_driver.h ----
// s_replay_feed      the replay task owns the ring's input; the USB
//                    callback discards live samples
// s_replay_phase     hand-over with the decoder task, which makes every
//                    change to decoder state itself, when the ring is empty:
//                    1 start requested -> 2 replaying -> 3 end requested -> 0
// s_replay_counting  the decoder is on replayed samples: count, and keep
//                    them out of MQTT, the SD log and the serial log
#define ADSB_REPLAY_TASK_STACK (6 * 1024)
#define ADSB_REPLAY_TASK_PRIO  3
static volatile bool s_replay_feed = false;
static volatile int  s_replay_phase = 0;
static volatile bool s_replay_counting = false;
static volatile bool s_replay_stop = false;
static volatile bool s_live_gap_pending = false;   // tell the decoder live samples restart here
static volatile uint32_t s_replay_msgs = 0;
static uint64_t s_replay_busy_us = 0;              // decoder task only
static adsb_replay_status_t s_replay;              // written by the replay task

// ---- Decoder options (adsb_decoder_opts_t in class_driver.h) ----
// s_dec_req is what was asked for, packed into one word so that a change
// is a single store; the decoder task copies it into the decoder state
// between two of its runs (decoder_apply_options) and notes in
// s_dec_applied what it runs with. Boot values from Kconfig
// (ADSB_DECODE_SOFT / _COH / _FIX2 in main/Kconfig.projbuild).
#define DEC_FIX2    0x100u
#define DEC_STRICT  0x200u
#define DEC_KNOWN   0x400u
#define DEC_COH     0x800u
#define DEC_SOFT(w) ((w) & 0xFFu)
#if defined(CONFIG_ADSB_DECODE_FIX2)
#define DEC_BOOT_FIX (DEC_FIX2 | DEC_STRICT | DEC_KNOWN | 8u)
#elif defined(CONFIG_ADSB_DECODE_SOFT)
#define DEC_BOOT_FIX (DEC_STRICT | DEC_KNOWN | 8u)
#else
#define DEC_BOOT_FIX 0u
#endif
#ifdef CONFIG_ADSB_DECODE_COH
#define DEC_BOOT_COH DEC_COH
#else
#define DEC_BOOT_COH 0u
#endif
static volatile uint32_t s_dec_req = DEC_BOOT_FIX | DEC_BOOT_COH;
static volatile uint32_t s_dec_applied = 0xFFFFFFFFu;   // nothing applied yet
static volatile bool     s_coh_active = false;          // the coherent look is really on
static void             *s_coh_work = NULL;             // its working memory (PSRAM); allocated on first use, kept
static volatile uint32_t s_decode_stack_free = 0;       // least free stack of the decoder task, bytes
// Decoder repair counters at three moments: the start and end of a replay
// (decoder task) and the last DECODER: log line.
typedef struct { uint32_t fix1, fix2, soft, coh_try, coh_abort, coh_ok; } dec_counts_t;
static dec_counts_t s_replay_cnt0, s_replay_cnt1, s_dec_line_cnt;
static int64_t s_dec_line_us = 0;                       // when s_dec_line_cnt was taken

static void dec_counts_now(dec_counts_t *c)
{
    c->fix1 = state.stat_fix1;
    c->fix2 = state.stat_fix2;
    c->soft = state.stat_soft_fix;
    c->coh_try = state.stat_coh_try;
    c->coh_abort = state.stat_coh_abort;
    c->coh_ok = state.stat_coh_ok;
}

static void dec_unpack(uint32_t v, adsb_decoder_opts_t *o)
{
    o->fix2 = (v & DEC_FIX2) != 0;
    o->strict = (v & DEC_STRICT) != 0;
    o->known_addr = (v & DEC_KNOWN) != 0;
    o->coh = (v & DEC_COH) != 0;
    o->soft = (uint8_t)(v & 0xFFu);
}

// ADS-B statistics — updated by on_msg, read by adsb_get_stats
static volatile uint32_t s_total_messages = 0;
static volatile uint32_t s_msg_count_window = 0;  // messages in current 1s window
static volatile float    s_msg_rate = 0.0f;       // messages per second
static volatile int64_t  s_msg_window_start = 0;  // start of current rate window
static volatile bool     s_rtlsdr_connected = false;
static volatile bool     s_rtlsdr_error = false;  // device seen but transfer buffer failed

// ============================================================
// Adaptive tuner gain — throughput-optimized two-phase
// ============================================================
// R820T valid gain steps in tenths of dB (from datasheet)
static const uint16_t R820T_GAINS[] = {
    0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
    166, 197, 207, 229, 254, 280, 297, 328, 338,
    364, 372, 386, 402, 421, 434, 439, 445, 480, 496
};
#define R820T_GAIN_COUNT (sizeof(R820T_GAINS) / sizeof(R820T_GAINS[0]))
#define GAIN_FLOOR_IDX      11     // 19.7 dB — never go below this
#define GAIN_MIN_SAMPLES    10     // need enough decoded messages to evaluate

// Phase 1 (fast convergence): eval every 10s or 200 msgs
#define GAIN_P1_PERIOD_US   10000000LL
#define GAIN_P1_MSG_COUNT   200
#define GAIN_P1_STABLE_WIN  3      // consecutive stable windows → converged
#define GAIN_WARMUP_US      10000000LL  // 10s decoder warmup before first eval

// Phase 2 (steady-state): eval every 60s
#define GAIN_P2_PERIOD_US   60000000LL
#define GAIN_P2_EMA_ALPHA   0.3f   // EMA smoothing factor
#define GAIN_P2_COOLDOWN    2      // skip 2 windows after gain change (120s settle)
#define GAIN_P2_THROUGHPUT_DROP 0.15f // >15% pos_rate drop = bad step (beyond traffic noise)

// Thresholds — the target band is user-configurable via settings.
// Derived thresholds maintain fixed offsets from the band edges.
// SEVERE is absolute (ADC clipping), not relative to the band.
#define GAIN_ERR_SEVERE     0.67f  // phase 2 emergency — fall back to phase 1

// Signal quality gates — prevent gain reductions when signals are already weak.
// Based on preamble signal metric (avg of 4 preamble peaks per CRC-ok message):
//   Clipping regime: sig > 10000 (e.g., 13411 at 49.6 dB)
//   Good range:      sig 5000–20000 (aircraft in range, ADC not saturated)
//   Weak range:      sig < 5000 (distant/departing aircraft, barely decoding)
// High error + weak signals = noise or no traffic, NOT ADC saturation.
#define GAIN_MIN_SIG_FOR_CLIPPING  8000  // P2 emergency: must see strong signals to claim clipping
#define GAIN_MIN_SIG_FOR_DOWN      5000  // P1/P2: don't reduce gain if signals weaker than this
#define GAIN_MIN_OK_FOR_STATS      3     // need ≥3 CRC-ok messages for reliable signal metrics
static inline float gain_err_high(void) { extern device_settings_t g_settings; return g_settings.gain_err_high / 100.0f; }
static inline float gain_err_low(void)  { extern device_settings_t g_settings; return g_settings.gain_err_low / 100.0f; }
static inline float gain_err_very_high(void) { return gain_err_high() + 0.05f; }  // clear soft floor
static inline float gain_err_very_low(void)  { return gain_err_low() - 0.20f; }   // clear soft ceiling

// Soft ceiling (can't step above)
#define GAIN_CEILING_CLEAR_WIN 5   // 5 consecutive low-error windows to clear ceiling
#define GAIN_CEILING_MAX_WIN   20  // hard time cap: 20 windows (20 min) then auto-clear
#define GAIN_CEILING_STRIKES   2   // require 2 failed probes before setting ceiling

// Soft floor (can't step below)
#define GAIN_FLOOR_CLEAR_WIN   5   // 5 consecutive high-error windows to clear floor
#define GAIN_FLOOR_MAX_WIN     20  // hard time cap: 20 windows (20 min) then auto-clear
#define GAIN_FLOOR_STRIKES     2   // require 2 failed probes before setting floor

// ── Core state ──
static volatile int      s_gain_idx = R820T_GAIN_COUNT - 1;
static volatile bool     s_gain_pending = false;  // deferred gain apply — set by external callers
static volatile bool     s_gain_auto = true;
static volatile int64_t  s_gain_last_eval = 0;

// ── Phase state ──
static volatile int      s_gain_phase = 1;     // 1=fast, 2=steady
static volatile int      s_gain_stable_count = 0; // consecutive windows without gain change
static volatile float    s_gain_p1_prev_pos_rate = 0.0f; // previous window pos_rate for phase 1 comparison
static volatile uint32_t s_gain_p1_prev_ok = 0;          // previous window ok (good CRC) count
static volatile bool     s_gain_p1_last_was_up = false;   // true if last phase 1 action was a step-up
static volatile int      s_gain_p1_up_suppress = 0;       // >0 = suppress step-ups (failed step-up cooldown)
static volatile bool     s_gain_warmup = true;             // true until first eval completes

// ── Per-window position update counter (incremented in on_msg) ──
static volatile uint32_t s_gain_win_pos = 0;

// ── Phase 2: EMA-smoothed metrics ──
static volatile float    s_gain_err_ema = 0.0f;     // error rate EMA
static volatile float    s_gain_pos_rate_ema = 0.0f; // position updates/sec EMA (primary metric)
static volatile float    s_gain_error_rate = 0.0f;   // exposed to stats API

// ── Signal quality metrics (last window, exposed to stats API) ──
static volatile uint32_t s_gain_avg_signal = 0;      // avg preamble signal level (CRC-ok msgs)
static volatile uint32_t s_gain_avg_delta = 0;        // avg bit delta / SNR proxy (CRC-ok msgs)

// ── Phase 2: throughput comparison ──
static volatile float    s_gain_pre_step_pos_rate = 0.0f; // snapshot before gain change
static volatile uint32_t s_gain_pre_step_signal = 0;      // avg_signal snapshot before gain change
static volatile int      s_gain_cooldown = 0;
static volatile int      s_gain_step_dir = 0;  // +1=stepped up, -1=stepped down, 0=none
#define GAIN_P2_SIG_CHANGE_THRESH 0.40f // >40% signal change = traffic shifted, not gain damage

// ── Soft ceiling ──
static volatile int      s_gain_ceiling = R820T_GAIN_COUNT; // one past max = no ceiling
static volatile int      s_gain_low_err_count = 0;
static volatile int      s_gain_ceiling_strikes = 0;  // failed probes since last ceiling clear
static volatile int      s_gain_ceiling_age = 0;       // windows since ceiling was set

// ── Soft floor ──
static volatile int      s_gain_floor = -1;            // -1 = no floor (GAIN_FLOOR_IDX is hard limit)
static volatile int      s_gain_high_err_count = 0;
static volatile int      s_gain_floor_strikes = 0;     // failed probes since last floor clear
static volatile int      s_gain_floor_age = 0;          // windows since floor was set

// ── Historical throughput profile (EMA of pos_rate per gain step) ──
#define GAIN_PROFILE_ALPHA    0.1f  // slow EMA — profile reflects hours, not minutes
#define GAIN_PROFILE_MIN_WIN  3     // need at least 3 windows before trusting a profile entry
#define GAIN_PROFILE_MIN_DELTA 0.25f // profile must be at least 0.25/s better (prevents noise probes)
static float    s_gain_profile[R820T_GAIN_COUNT];      // EMA pos_rate at each level
static uint16_t s_gain_profile_samples[R820T_GAIN_COUNT]; // windows observed at each level

// ── Gain from levels and yield (gain_ctl.c) – the loop used with the 2.4 Msps
// demodulator, for which the error rate above carries no information about
// the gain (docs/DECODER.md section 5.8). The loop state and the level
// histogram the decoder fills live in one PSRAM block. Everything here is
// touched by the decoder task only, except the request words, which other
// tasks write and the decoder task takes over between two runs.
typedef struct {
    gain_ctl_t ctl;
    uint32_t   hist[MODE_S_LVL_BINS];
} gain_level_mem_t;
static gain_level_mem_t *s_gl = NULL;
#define GL_REQ_RESTART_MAX  1u    // adaptive gain (re)started from the top
#define GL_REQ_RESTART_KEEP 2u    // ... from the gain in use
static volatile uint32_t s_gl_req = 0;
static int64_t s_gl_settle_until = 0;             // a gain change is settling: no window is open
// Level figures of the last window, for adsb_get_stats()
static volatile float s_gain_noise = 0.0f, s_gain_headroom = 0.0f, s_gain_loud = -1.0f, s_gain_clip = 0.0f;
// Sweep (`gain sweep`): hold each gain step for a while and print what it yields
static volatile int32_t s_sweep_req = 0;          // > 0: start one with this many seconds a step;
                                                  // -1: stop and go back; -2: stop, the gain was set by hand
static volatile bool s_sweep_running = false;     // for other tasks to see
static struct {
    bool    active;
    int     idx;            // step being measured
    int     saved_idx;      // what to go back to
    int64_t t0;             // start of the measurement at this step
    int64_t dwell_us;
} s_sweep;

// ============================================================
// CPR cache with timestamps — linear-probed hash table
// ============================================================

typedef struct {
    uint32_t icao;
    int even_valid, odd_valid;
    int even_lat, even_lon;
    int odd_lat, odd_lon;
    int64_t even_ts, odd_ts;
    double lat, lon;
    int occupied;
} cpr_cache_t;

static cpr_cache_t cpr_cache[CPR_CACHE_SIZE];

static cpr_cache_t *cpr_get(uint32_t icao) {
    uint32_t idx = icao % CPR_CACHE_SIZE;

    // Linear probe: find existing entry or first free slot
    for (uint32_t probe = 0; probe < CPR_CACHE_SIZE; probe++) {
        uint32_t slot = (idx + probe) % CPR_CACHE_SIZE;

        if (cpr_cache[slot].occupied && cpr_cache[slot].icao == icao)
            return &cpr_cache[slot];

        if (!cpr_cache[slot].occupied) {
            memset(&cpr_cache[slot], 0, sizeof(cpr_cache_t));
            cpr_cache[slot].icao = icao;
            cpr_cache[slot].occupied = 1;
            return &cpr_cache[slot];
        }
    }

    // Table full — evict the slot with the oldest timestamp
    uint32_t oldest_slot = idx;
    int64_t oldest_ts = INT64_MAX;
    for (uint32_t i = 0; i < CPR_CACHE_SIZE; i++) {
        int64_t ts = cpr_cache[i].even_ts > cpr_cache[i].odd_ts
                   ? cpr_cache[i].even_ts : cpr_cache[i].odd_ts;
        if (ts < oldest_ts) {
            oldest_ts = ts;
            oldest_slot = i;
        }
    }
    memset(&cpr_cache[oldest_slot], 0, sizeof(cpr_cache_t));
    cpr_cache[oldest_slot].icao = icao;
    cpr_cache[oldest_slot].occupied = 1;
    return &cpr_cache[oldest_slot];
}

static int cpr_decode(cpr_cache_t *c) {
    #define NL(lat) ((lat) == 0 ? 59 : (int)(2*M_PI / acos(1 - (1-cos(M_PI/30.0)) / pow(cos(M_PI/180.0*(lat)),2))))

    // Reject stale pairs
    int64_t age = c->even_ts - c->odd_ts;
    if (age < 0) age = -age;
    if (age > CPR_MAX_AGE_US) return 0;

    double lat_even = c->even_lat / 131072.0;
    double lat_odd  = c->odd_lat  / 131072.0;

    int j = (int)floor(59 * lat_even - 60 * lat_odd + 0.5);

    double rlat_even = (360.0 / 60) * (((j % 60) + 60) % 60 + lat_even);
    double rlat_odd  = (360.0 / 59) * (((j % 59) + 59) % 59 + lat_odd);
    if (rlat_even >= 270) rlat_even -= 360;
    if (rlat_odd  >= 270) rlat_odd  -= 360;

    if (NL(rlat_even) != NL(rlat_odd)) return 0;

    // Use the most recently received frame
    double rlat = (c->even_ts >= c->odd_ts) ? rlat_even : rlat_odd;
    int nl = NL(rlat);

    double lon_even = c->even_lon / 131072.0;
    double lon_odd  = c->odd_lon  / 131072.0;

    int ni_even = nl > 1 ? nl : 1;
    int ni_odd  = (nl - 1) > 1 ? (nl - 1) : 1;

    int m;
    double rlon;
    if (c->even_ts >= c->odd_ts) {
        m    = (int)floor(lon_even * (nl - 1) - lon_odd * nl + 0.5);
        rlon = (360.0 / ni_even) * (((m % ni_even) + ni_even) % ni_even + lon_even);
    } else {
        m    = (int)floor(lon_even * (nl - 1) - lon_odd * nl + 0.5);
        rlon = (360.0 / ni_odd) * (((m % ni_odd) + ni_odd) % ni_odd + lon_odd);
    }
    if (rlon >= 180) rlon -= 360;

    c->lat = rlat;
    c->lon = rlon;
    return 1;
}

// Local CPR decode: decode a single CPR frame against a known reference position.
// Works when we already have a valid position from a previous global decode.
// The aircraft must be within ~180 nm of the reference — always true for recent positions.
static double fmod_pos(double a, double b) {
    double r = fmod(a, b);
    return r < 0 ? r + b : r;
}

static int cpr_decode_local(double ref_lat, double ref_lon, int raw_lat, int raw_lon,
                            int fflag, double *out_lat, double *out_lon) {
    double d_lat = fflag ? (360.0 / 59.0) : (360.0 / 60.0);
    double cprlat = raw_lat / 131072.0;
    double cprlon = raw_lon / 131072.0;

    // Latitude
    int j = (int)floor(ref_lat / d_lat) +
            (int)floor(0.5 + fmod_pos(ref_lat, d_lat) / d_lat - cprlat);
    double rlat = d_lat * (j + cprlat);

    // Sanity check latitude
    if (rlat < -90 || rlat > 90) return 0;

    // Longitude
    int nl = NL(rlat);
    int ni = fflag ? ((nl - 1) > 1 ? (nl - 1) : 1) : (nl > 1 ? nl : 1);
    double d_lon = 360.0 / ni;

    int m = (int)floor(ref_lon / d_lon) +
            (int)floor(0.5 + fmod_pos(ref_lon, d_lon) / d_lon - cprlon);
    double rlon = d_lon * (m + cprlon);

    if (rlon >= 180) rlon -= 360;
    if (rlon < -180) rlon += 360;

    // Sanity: reject if decoded position is >50 nm from reference
    // (local decode is valid within ~180 nm, but 50 nm catches errors early)
    double dlat = rlat - ref_lat;
    double dlon = (rlon - ref_lon) * cos(ref_lat * M_PI / 180.0);
    double dist_nm = sqrt(dlat * dlat + dlon * dlon) * 60.0;
    if (dist_nm > 50.0) return 0;

    *out_lat = rlat;
    *out_lon = rlon;
    return 1;
}

// ============================================================
// Aircraft state table
// ============================================================

typedef struct {
    uint32_t icao;
    char callsign[9];
    int altitude;
    int speed;
    int heading;
    int vert_rate;
    double lat, lon;
    int squawk;
    int has_position;
    int64_t last_seen;
    int active;
    uint32_t msg_count;
    // Position validation
    double good_lat, good_lon;  // last accepted (non-suspect) position
    int64_t good_pos_ts;        // timestamp of last good position
    int position_suspect;       // 1 = current lat/lon may be wrong, awaiting recovery
    int suspect_count;          // consecutive suspect events (give up after N)
    // Consensus recovery: track agreeing global decodes during suspect mode
    double suspect_lat, suspect_lon;  // last candidate during recovery
    int suspect_agree;                // consecutive globals agreeing with each other
} aircraft_t;

static aircraft_t aircraft_table[AIRCRAFT_TABLE_SIZE];
static SemaphoreHandle_t aircraft_mutex = NULL;

static aircraft_t *aircraft_get(uint32_t icao) {
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++)
        if (aircraft_table[i].active && aircraft_table[i].icao == icao)
            return &aircraft_table[i];

    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
        if (!aircraft_table[i].active) {
            memset(&aircraft_table[i], 0, sizeof(aircraft_t));
            aircraft_table[i].icao = icao;
            aircraft_table[i].active = 1;
            return &aircraft_table[i];
        }
    }

    // Evict oldest
    int oldest = 0;
    for (int i = 1; i < AIRCRAFT_TABLE_SIZE; i++)
        if (aircraft_table[i].last_seen < aircraft_table[oldest].last_seen)
            oldest = i;
    memset(&aircraft_table[oldest], 0, sizeof(aircraft_t));
    aircraft_table[oldest].icao = icao;
    aircraft_table[oldest].active = 1;
    return &aircraft_table[oldest];
}

static void aircraft_expire(void) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++)
        if (aircraft_table[i].active &&
            (now - aircraft_table[i].last_seen) > AIRCRAFT_MAX_AGE_US)
            aircraft_table[i].active = 0;
}

// Check if a candidate position is a plausible jump from the last known good position.
// Returns 1 if plausible, 0 if implausible.
static int cpr_jump_check(aircraft_t *ac, double cand_lat, double cand_lon, int64_t now) {
    if (!ac->good_pos_ts) return 1;  // no reference — first position, always accept

    double elapsed_s = (double)(now - ac->good_pos_ts) / 1000000.0;
    if (elapsed_s < 0.1) elapsed_s = 0.1;  // avoid division by zero

    double max_nm = CPR_JUMP_FLOOR_NM + elapsed_s * CPR_JUMP_RATE_NM_S;

    double dlat = cand_lat - ac->good_lat;
    double dlon = (cand_lon - ac->good_lon) * cos(ac->good_lat * M_PI / 180.0);
    double dist_nm = sqrt(dlat * dlat + dlon * dlon) * 60.0;

    return dist_nm <= max_nm;
}

// ============================================================
// Receiver position (fed by GPS task)
// ============================================================

static receiver_pos_t s_rx_pos = {0};
static SemaphoreHandle_t s_rx_pos_mutex = NULL;

static void rx_pos_init(void) {
    if (s_rx_pos_mutex == NULL)
        s_rx_pos_mutex = xSemaphoreCreateMutex();
}

void adsb_set_receiver_pos(double lat, double lon, double alt_m,
                          int sats, double hdop, int fix_quality) {
    if (!s_rx_pos_mutex) return;
    xSemaphoreTake(s_rx_pos_mutex, portMAX_DELAY);
    s_rx_pos.lat = lat;
    s_rx_pos.lon = lon;
    s_rx_pos.alt_m = alt_m;
    s_rx_pos.fix_valid = 1;
    s_rx_pos.sats = sats;
    s_rx_pos.hdop = hdop;
    s_rx_pos.fix_quality = fix_quality;
    xSemaphoreGive(s_rx_pos_mutex);
}

receiver_pos_t adsb_get_receiver_pos(void) {
    receiver_pos_t p = {0};
    if (!s_rx_pos_mutex) return p;
    xSemaphoreTake(s_rx_pos_mutex, portMAX_DELAY);
    p = s_rx_pos;
    xSemaphoreGive(s_rx_pos_mutex);
    return p;
}

void adsb_set_bias_tee(bool on) {
    // A USB control transfer: handed to the reader task, which owns the
    // device while samples are streaming.
    if (rtldev) {
        s_bias_pending = on ? 1 : 0;
    }
}

// Max range — updated by adsb_get_stats(), referenced by gain logs
static volatile double s_max_range_nm = 0.0;

adsb_stats_t adsb_get_stats(void) {
    adsb_stats_t stats = {0};
    stats.total_messages = s_total_messages;
    stats.msg_rate = s_msg_rate;
    stats.rtlsdr_connected = s_rtlsdr_connected;
    stats.rtlsdr_error = s_rtlsdr_error;

    // Count active aircraft and find the nearest and the farthest. Under
    // the lock only a ranking value is computed for each (single
    // precision, three trigonometric calls); the two distances are worked
    // out after it is released. Up to coh3 this was a double-precision
    // haversine per aircraft inside the lock, which the decoder needs for
    // every message.
    int count = 0;
    int64_t now = esp_timer_get_time();
    stats.nearest_icao = 0;
    stats.nearest_dist_nm = 0;
    stats.farthest_dist_nm = 0;
    stats.farthest_icao = 0;
    receiver_pos_t rx = adsb_get_receiver_pos();
    double near_lat = 0, near_lon = 0, far_lat = 0, far_lon = 0;
    bool have_near = false, have_far = false;
    if (aircraft_mutex) {
        const float cos_rx = rx.fix_valid ? adsb_gc_cos(rx.lat) : 0.0f;
        float best = 9.0f, worst = -1.0f;
        xSemaphoreTake(aircraft_mutex, portMAX_DELAY);
        for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
            const aircraft_t *a = &aircraft_table[i];
            if (!a->active) continue;
            if ((now - a->last_seen) > AIRCRAFT_MAX_AGE_US) continue;
            count++;
            if (!rx.fix_valid || !a->has_position) continue;
            const float rank = adsb_gc_rank(rx.lat, rx.lon, cos_rx, a->lat, a->lon);
            if (rank < best) {
                best = rank;
                have_near = true;
                near_lat = a->lat;
                near_lon = a->lon;
                stats.nearest_icao = a->icao;
                stats.nearest_alt = a->altitude;
                // (up to coh3 an aircraft without a callsign was shown
                // with the callsign of the previous candidate)
                strncpy(stats.nearest_callsign, a->callsign, 8);
                stats.nearest_callsign[8] = '\0';
            }
            if (rank > worst) {
                worst = rank;
                have_far = true;
                far_lat = a->lat;
                far_lon = a->lon;
                stats.farthest_icao = a->icao;
            }
        }
        xSemaphoreGive(aircraft_mutex);
    }
    stats.active_aircraft = count;
    if (have_near) {
        float km, brg;
        adsb_gc(rx.lat, rx.lon, near_lat, near_lon, &km, &brg);
        stats.nearest_dist_nm = km * 0.539957f;
    }
    if (have_far) {
        float km, brg;
        adsb_gc(rx.lat, rx.lon, far_lat, far_lon, &km, &brg);
        stats.farthest_dist_nm = km * 0.539957f;
    }

    stats.gain_tenths = R820T_GAINS[s_gain_idx];
    stats.gain_auto = s_gain_auto;
    stats.gain_phase = s_gain_phase;
    stats.crc_error_rate = s_gain_error_rate;
    stats.pos_rate = s_gain_pos_rate_ema;
    stats.avg_signal = s_gain_avg_signal;
    stats.avg_delta = s_gain_avg_delta;
    stats.gain_by_levels = ADSB_DEMOD_IS_2400 != 0;
    stats.gain_sweep = s_sweep_running;
    stats.noise_counts = s_gain_noise;
    stats.headroom_db = s_gain_headroom;
    stats.loud_share = s_gain_loud;
    stats.clip_share = s_gain_clip;
    stats.df18_total = s_df18_total;
    stats.df18_last_min = s_df18_last_min;
    stats.df18_tisb = s_df18_tisb;
    stats.df18_adsr = s_df18_adsr;
    stats.df18_adsb_nt = s_df18_adsb_nt;
    stats.df18_other = s_df18_other;
    stats.decode_load = s_decode_load_permille / 1000.0f;
    stats.sdlog_load  = s_sdlog_load_permille / 1000.0f;
    stats.capture     = s_capture_permille / 1000.0f;
    stats.usb_capture = s_usb_capture_permille / 1000.0f;
    stats.min_capture = s_min_capture_permille / 1000.0f;
    stats.peak_decode_load = s_peak_decode_permille / 1000.0f;
    if (s_ring_mem) {
        stats.ring_fill_pct = sample_ring_fill(&s_ring) * 100 / (ADSB_RING_SLOTS - 1);
        stats.ring_max_pct  = s_ring.max_fill * 100 / (ADSB_RING_SLOTS - 1);
        stats.ring_dropped  = s_ring.dropped;
    }
    stats.usb_xfers    = (uint32_t)s_usb_xfers;
    stats.usb_starved  = s_usb_starved;
    stats.stream_gaps  = s_decode_gaps;
    stats.log_lines_s  = s_log_lines_s;
    stats.log_bytes_s  = s_log_bytes_s;
    stats.log_dropped  = s_log_dropped_total;
    stats.log_dropped_win = s_log_dropped_win;
    stats.mark_age_s        = (uint32_t)((esp_timer_get_time() - s_mark_us) / 1000000LL);
    stats.ring_dropped_mark = (s_ring_mem ? s_ring.dropped : 0) - s_mark_ring_dropped;
    stats.usb_starved_mark  = s_usb_starved - s_mark_usb_starved;
    stats.log_dropped_mark  = s_log_dropped_total - s_mark_log_dropped;
    s_max_range_nm = stats.farthest_dist_nm;

    /* Build human-readable dongle model string for UI/MQTT */
    stats.dongle_model[0] = '\0';
    if (rtldev) {
        rtlsdr_get_dongle_info(rtldev, stats.dongle_model, sizeof(stats.dongle_model),
                               NULL, NULL);
    } else {
        snprintf(stats.dongle_model, sizeof(stats.dongle_model), "not connected");
    }

    return stats;
}

// ── Gain control ──────────────────────────────────────────────────────────

// Find the closest valid R820T gain index for a given tenths-of-dB value
static int gain_find_idx(uint16_t tenths) {
    int best = 0;
    int best_diff = abs((int)tenths - (int)R820T_GAINS[0]);
    for (int i = 1; i < (int)R820T_GAIN_COUNT; i++) {
        int diff = abs((int)tenths - (int)R820T_GAINS[i]);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

// Apply the current gain index to the hardware
static void gain_apply_now(void) {
    if (rtldev) {
        rtlsdr_set_tuner_gain(rtldev, R820T_GAINS[s_gain_idx]);
    }
}

// Tuner writes are USB control transfers and belong to the reader task,
// which owns the device while samples are streaming. Called from there it
// writes at once; from anywhere else (the decoder's adaptive gain loop)
// it leaves the request for the reader, which picks it up within 20 ms.
static void gain_apply(void) {
    if (s_adsb_reader_task_hdl != NULL && xTaskGetCurrentTaskHandle() == s_adsb_reader_task_hdl) {
        gain_apply_now();
    } else {
        s_gain_pending = true;
    }
}

// Set gain mode and value from external callers (serial console, settings)
void adsb_set_gain(int mode, uint16_t gain_tenths) {
    if (mode == 0) {
        // Auto (adaptive) — restart from phase 1
        s_gain_auto = true;
        s_gain_phase = 1;
        s_gain_stable_count = 0;
        s_gain_p1_prev_pos_rate = 0.0f;
        s_gain_p1_prev_ok = 0;
        s_gain_p1_last_was_up = false;
        s_gain_p1_up_suppress = 0;
        s_gain_warmup = true;
        s_gain_err_ema = 0.0f;
        s_gain_pos_rate_ema = 0.0f;
        s_gain_cooldown = 0;
        s_gain_step_dir = 0;
        s_gain_ceiling = R820T_GAIN_COUNT;
        s_gain_low_err_count = 0;
        s_gain_ceiling_strikes = 0;
        s_gain_ceiling_age = 0;
        s_gain_floor = -1;
        s_gain_high_err_count = 0;
        s_gain_floor_strikes = 0;
        s_gain_floor_age = 0;
        s_gain_win_pos = 0;
        memset((void *)s_gain_profile, 0, sizeof(s_gain_profile));
        memset((void *)s_gain_profile_samples, 0, sizeof(s_gain_profile_samples));
        s_gain_idx = R820T_GAIN_COUNT - 1; // start at max
        s_gain_pending = true;  // deferred — USB task will apply
        s_gl_req = GL_REQ_RESTART_MAX;
        if (s_sweep_running) s_sweep_req = -2;
        g_settings.adsb_gain_mode = 0;
        char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
        serial_console_print("%sGAIN: Switched to adaptive gain (starting at %.1f dB, phase 1)\n",
                             _ts, R820T_GAINS[s_gain_idx] / 10.0);
    } else {
        // Manual (fixed)
        s_gain_auto = false;
        s_gain_idx = gain_find_idx(gain_tenths);
        s_gl_req = GL_REQ_RESTART_KEEP;   // a fresh window at the new gain
        if (s_sweep_running) s_sweep_req = -2;
        g_settings.adsb_gain_mode = 1;
        g_settings.adsb_gain_tenths = R820T_GAINS[s_gain_idx];
        s_gain_pending = true;  // deferred — USB task will apply
        char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
        serial_console_print("%sGAIN: Set manual gain: %.1f dB (step %d/%d)\n",
                             _ts, R820T_GAINS[s_gain_idx] / 10.0,
                             s_gain_idx, (int)R820T_GAIN_COUNT - 1);
    }
    g_settings_save_pending = true;
}

// See class_driver.h.
bool adsb_gain_sweep(int seconds) {
    if (!ADSB_DEMOD_IS_2400) return false;
    if (seconds <= 0) {
        if (s_sweep_running) s_sweep_req = -1;
        return true;
    }
    if (seconds > 600) seconds = 600;
    s_sweep_req = seconds;
    return true;
}

// Restart adaptive gain from current level — re-enters P1 without resetting to max.
// Clears ceiling/floor/EMA (stale environmental context) but keeps the current gain
// setting. Use after antenna changes or environment shifts.
void adsb_gain_restart(void) {
    if (!s_gain_auto) return;  // only meaningful in auto mode

    int old_idx = s_gain_idx;  // preserve current gain
    s_gain_phase = 1;
    s_gain_stable_count = 0;
    s_gain_p1_prev_pos_rate = 0.0f;
    s_gain_p1_prev_ok = 0;
    s_gain_p1_last_was_up = false;
    s_gain_p1_up_suppress = 0;
    s_gain_warmup = false;  // no warmup — decoder is already running
    s_gain_err_ema = 0.0f;
    s_gain_pos_rate_ema = 0.0f;
    s_gain_cooldown = 0;
    s_gain_step_dir = 0;
    s_gain_ceiling = R820T_GAIN_COUNT;
    s_gain_low_err_count = 0;
    s_gain_ceiling_strikes = 0;
    s_gain_ceiling_age = 0;
    s_gain_floor = -1;
    s_gain_high_err_count = 0;
    s_gain_floor_strikes = 0;
    s_gain_floor_age = 0;
    s_gain_win_pos = 0;
    s_gl_req = GL_REQ_RESTART_KEEP;
    // Keep gain profile — historical data is still useful
    // Keep s_gain_idx — don't reset to max

    char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
    serial_console_print("%sGAIN: Restart from %.1f dB — re-entering phase 1\n",
                         _ts, R820T_GAINS[old_idx] / 10.0);
}

// Target dB reductions for phase 1 proportional response (in tenths of dB).
// These are consistent power reductions regardless of where we are in the
// non-uniform R820T gain table.  dB is logarithmic: 3 dB = ½ power.
#define GAIN_DB_DROP_EXTREME  60   // >90% error: ~6 dB (power ÷ 4)
#define GAIN_DB_DROP_SEVERE   45   // 80-90%: ~4.5 dB (power ÷ 2.8)
#define GAIN_DB_DROP_MODERATE 30   // 60-80%: ~3 dB (power ÷ 2)
#define GAIN_DB_DROP_MILD     15   // 50-60%: ~1.5 dB (power ÷ 1.4)

// Phase 2 step-up dB targets — graduated by how far below target band
#define GAIN_DB_RAISE_MILD     10  // 30-40% EMA: +1.0 dB
#define GAIN_DB_RAISE_MODERATE 15  // 20-30% EMA: +1.5 dB
#define GAIN_DB_RAISE_STRONG   25  // 10-20% EMA: +2.5 dB
#define GAIN_DB_RAISE_FULL     30  // ≤10% EMA:   +3.0 dB

// Find the gain index closest to (current - reduction_tenths), clamped to floor.
// Returns the index whose gain value is nearest to the target without going
// below GAIN_FLOOR_IDX.  Always moves at least 1 step if above floor.
static int gain_idx_for_db_drop(int current_idx, int reduction_tenths) {
    int target_val = R820T_GAINS[current_idx] - reduction_tenths;
    if (target_val < (int)R820T_GAINS[GAIN_FLOOR_IDX]) target_val = R820T_GAINS[GAIN_FLOOR_IDX];

    int best_idx = GAIN_FLOOR_IDX;
    int best_dist = abs((int)R820T_GAINS[GAIN_FLOOR_IDX] - target_val);
    for (int i = GAIN_FLOOR_IDX + 1; i < current_idx; i++) {
        int dist = abs((int)R820T_GAINS[i] - target_val);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    // Ensure we always move at least 1 step
    if (best_idx >= current_idx && current_idx > GAIN_FLOOR_IDX) {
        best_idx = current_idx - 1;
    }
    return best_idx;
}

// Find the gain index closest to (current + raise_tenths), clamped to max.
// Returns the index whose gain value is nearest to the target without going
// above R820T_GAIN_COUNT-1.  Always moves at least 1 step if below max.
static int gain_idx_for_db_raise(int current_idx, int raise_tenths) {
    int max_idx = (int)R820T_GAIN_COUNT - 1;
    int target_val = R820T_GAINS[current_idx] + raise_tenths;
    if (target_val > (int)R820T_GAINS[max_idx]) target_val = R820T_GAINS[max_idx];

    int best_idx = max_idx;
    int best_dist = abs((int)R820T_GAINS[max_idx] - target_val);
    for (int i = current_idx + 1; i < max_idx; i++) {
        int dist = abs((int)R820T_GAINS[i] - target_val);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    // Ensure we always move at least 1 step
    if (best_idx <= current_idx && current_idx < max_idx) {
        best_idx = current_idx + 1;
    }
    return best_idx;
}

// Throughput-optimized adaptive gain evaluation
// Primary metric: position updates per second (the actual useful output)
// Phase 1: graduated dB-targeted response — consistent power reductions
//   >90%: -6 dB, 80-90%: -4.5 dB, 67-80%: -3 dB, 50-67%: -1.5 dB (throughput-gated)
//   Target band: 40-50% error. Below 40%: step up (accelerated if confirmed).
// Phase 2: EMA smoothing, graduated dB-targeted step-ups, probe-and-compare
//   Emergency brake at >67% EMA → fall back to phase 1
static void gain_evaluate(mode_s_t *st) {
    uint32_t ok   = st->stat_crc_ok;
    uint32_t fail = st->stat_crc_fail;
    uint32_t total = ok + fail;
    uint32_t pos  = s_gain_win_pos;

    // Read signal quality metrics (accumulated for CRC-ok messages only)
    uint32_t avg_signal = ok > 0 ? (uint32_t)(st->stat_signal_sum / ok) : 0;
    uint32_t avg_delta  = ok > 0 ? (uint32_t)(st->stat_delta_sum / ok) : 0;

    // Reset counters for next window
    st->stat_crc_ok = 0;
    st->stat_crc_fail = 0;
    st->stat_preambles = 0;
    st->stat_signal_sum = 0;
    st->stat_delta_sum = 0;
    s_gain_win_pos = 0;

    // ── Minimum message guards ──
    if (total == 0) {
        // Zero messages: might be deaf, or just no traffic.
        // P1: step up — try to find signal. P2: hold — we were working, wait it out.
        if (s_gain_phase == 1 && s_gain_idx + 1 < (int)R820T_GAIN_COUNT
            && s_gain_idx + 1 < s_gain_ceiling) {
            s_gain_idx++;
            s_gain_stable_count = 0;
            gain_apply();
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P1] Up: %.1f dB (no messages — increasing sensitivity)\n",
                                 _ts, R820T_GAINS[s_gain_idx] / 10.0);
        }
        return;
    }
    if (total < GAIN_MIN_SAMPLES) return;  // too few messages for reliable evaluation

    float err_rate = (float)fail / (float)total;

    // Compute window duration for rate calculations
    int64_t now = esp_timer_get_time();
    float win_secs = (float)(now - s_gain_last_eval) / 1000000.0f;
    if (win_secs < 1.0f) win_secs = 1.0f;  // avoid division by zero
    float pos_rate = (float)pos / win_secs;

    int old_idx = s_gain_idx;

    // Store signal quality for stats API
    s_gain_avg_signal = avg_signal;
    s_gain_avg_delta = avg_delta;

    if (s_gain_phase == 1) {
        // ══════════════════════════════════════════════════════════
        // Phase 1: Fast convergence — dB-targeted proportional response
        // Uses consistent power reductions regardless of non-uniform gain table.
        // >90%: -6 dB (power ÷ 4)
        // 80-90%: -4.5 dB (power ÷ 2.8)
        // 67-80%: -3 dB (power ÷ 2)
        // 50-67%: -1.5 dB (gentle, with throughput check)
        // 40-50%: target range — sweet spot for coverage (~45% ideal)
        // <40%: step up — confirmed step-ups accelerate to +1.5 dB
        //        step-ups that hurt throughput → hold (overshot)
        // Converge: 3 consecutive windows without a gain change
        // ══════════════════════════════════════════════════════════
        s_gain_error_rate = err_rate;
        bool changed = false;
        if (s_gain_p1_up_suppress > 0) s_gain_p1_up_suppress--;

        // Signal quality gate: only reduce gain if we have enough CRC-ok
        // messages with strong signal levels to confirm ADC saturation.
        // Weak/absent signals + high error = noise or no traffic.
        bool can_reduce = (ok >= GAIN_MIN_OK_FOR_STATS
                           && avg_signal >= GAIN_MIN_SIG_FOR_DOWN);

        if (err_rate > 0.90f && s_gain_idx > GAIN_FLOOR_IDX && can_reduce) {
            // Near-total garbage — drop ~6 dB (power ÷ 4)
            int new_idx = gain_idx_for_db_drop(s_gain_idx, GAIN_DB_DROP_EXTREME);
            s_gain_idx = new_idx;
            changed = true;
            s_gain_p1_last_was_up = false;
            s_gain_p1_up_suppress = 0;
            gain_apply();
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P1] Down: %.1f → %.1f dB (−%.1f dB, err=%.0f%%, pos=%.1f/s, ok=%lu, sig=%lu delta=%lu)\n",
                                 _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                 (R820T_GAINS[old_idx] - R820T_GAINS[s_gain_idx]) / 10.0,
                                 err_rate * 100, pos_rate, (unsigned long)ok,
                                 (unsigned long)avg_signal, (unsigned long)avg_delta);
        } else if (err_rate > 0.80f && s_gain_idx > GAIN_FLOOR_IDX && can_reduce) {
            // Severe clipping — drop ~4.5 dB (power ÷ 2.8)
            int new_idx = gain_idx_for_db_drop(s_gain_idx, GAIN_DB_DROP_SEVERE);
            s_gain_idx = new_idx;
            changed = true;
            s_gain_p1_last_was_up = false;
            s_gain_p1_up_suppress = 0;
            gain_apply();
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P1] Down: %.1f → %.1f dB (−%.1f dB, err=%.0f%%, pos=%.1f/s, ok=%lu, sig=%lu delta=%lu)\n",
                                 _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                 (R820T_GAINS[old_idx] - R820T_GAINS[s_gain_idx]) / 10.0,
                                 err_rate * 100, pos_rate, (unsigned long)ok,
                                 (unsigned long)avg_signal, (unsigned long)avg_delta);
        } else if (err_rate > GAIN_ERR_SEVERE && s_gain_idx > GAIN_FLOOR_IDX && can_reduce) {
            // Significant clipping (67-80%) — drop ~3 dB
            int new_idx = gain_idx_for_db_drop(s_gain_idx, GAIN_DB_DROP_MODERATE);
            s_gain_idx = new_idx;
            changed = true;
            s_gain_p1_last_was_up = false;
            s_gain_p1_up_suppress = 0;
            gain_apply();
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P1] Down: %.1f → %.1f dB (−%.1f dB, err=%.0f%%, pos=%.1f/s, ok=%lu, sig=%lu delta=%lu)\n",
                                 _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                 (R820T_GAINS[old_idx] - R820T_GAINS[s_gain_idx]) / 10.0,
                                 err_rate * 100, pos_rate, (unsigned long)ok,
                                 (unsigned long)avg_signal, (unsigned long)avg_delta);
        } else if (err_rate > gain_err_high() && s_gain_idx > GAIN_FLOOR_IDX && can_reduce) {
            // Saturation starting (50-67%) — drop ~1.5 dB only if good message count holds
            // Fix: use ok count (good CRC messages) not pos_rate — pos_rate is confounded
            // by traffic changes (aircraft entering/leaving range between windows).
            if (s_gain_p1_prev_ok == 0 || ok >= (uint32_t)(s_gain_p1_prev_ok * 0.80f)) {
                int new_idx = gain_idx_for_db_drop(s_gain_idx, GAIN_DB_DROP_MILD);
                s_gain_idx = new_idx;
                changed = true;
                s_gain_p1_last_was_up = false;
                s_gain_p1_up_suppress = 0;
                gain_apply();
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P1] Down: %.1f → %.1f dB (−%.1f dB, err=%.0f%%, ok=%lu, prev_ok=%lu, pos=%.1f/s, sig=%lu delta=%lu)\n",
                                     _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                     (R820T_GAINS[old_idx] - R820T_GAINS[s_gain_idx]) / 10.0,
                                     err_rate * 100, (unsigned long)ok, (unsigned long)s_gain_p1_prev_ok, pos_rate,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
            } else {
                // Good messages dropped >20% — we're losing real aircraft, hold
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P1] Holding %.1f dB — ok dropped (err=%.0f%%, ok=%lu, prev_ok=%lu, pos=%.1f/s, sig=%lu delta=%lu)\n",
                                     _ts, R820T_GAINS[s_gain_idx] / 10.0, err_rate * 100,
                                     (unsigned long)ok, (unsigned long)s_gain_p1_prev_ok, pos_rate,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
                s_gain_p1_last_was_up = false;
            }
        } else if (err_rate < gain_err_low() && s_gain_idx < (int)R820T_GAIN_COUNT - 1
                   && s_gain_p1_up_suppress == 0) {
            // Low error (<40%) — we're deaf to distant aircraft, step up
            // If last action was also a step-up that improved ok count and
            // we're still below the saturation threshold, take a 1.5 dB step.
            // Otherwise, conservative single-index step.
            if (s_gain_p1_last_was_up && s_gain_p1_prev_ok > 0
                && ok >= s_gain_p1_prev_ok) {
                // Confirmed: last step-up helped. Accelerate with 1.5 dB step.
                int new_idx = gain_idx_for_db_raise(s_gain_idx, GAIN_DB_DROP_MILD);
                s_gain_idx = new_idx;
                changed = true;
                s_gain_p1_last_was_up = true;
                gain_apply();
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P1] Up (accel): %.1f → %.1f dB (+%.1f dB, err=%.0f%%, ok=%lu, prev_ok=%lu, pos=%.1f/s, sig=%lu delta=%lu)\n",
                                     _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                     (R820T_GAINS[s_gain_idx] - R820T_GAINS[old_idx]) / 10.0,
                                     err_rate * 100, (unsigned long)ok, (unsigned long)s_gain_p1_prev_ok, pos_rate,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
            } else if (s_gain_p1_last_was_up && s_gain_p1_prev_ok > 0
                       && ok < (uint32_t)(s_gain_p1_prev_ok * 0.80f)) {
                // Last step-up hurt throughput — we overshot
                // Suppress further step-ups to allow convergence at this gain
                s_gain_p1_up_suppress = GAIN_P1_STABLE_WIN;
                s_gain_p1_last_was_up = false;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P1] Step-up hurt throughput — suppressing (err=%.0f%%, ok=%lu, prev_ok=%lu, pos=%.1f/s, sig=%lu delta=%lu)\n",
                                     _ts, err_rate * 100, (unsigned long)ok, (unsigned long)s_gain_p1_prev_ok, pos_rate,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
            } else {
                // First step-up or throughput ambiguous — conservative single step
                s_gain_idx++;
                changed = true;
                s_gain_p1_last_was_up = true;
                gain_apply();
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P1] Up: %.1f → %.1f dB (+%.1f dB, err=%.0f%%, ok=%lu, prev_ok=%lu, pos=%.1f/s, sig=%lu delta=%lu)\n",
                                     _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                     (R820T_GAINS[s_gain_idx] - R820T_GAINS[old_idx]) / 10.0,
                                     err_rate * 100, (unsigned long)ok, (unsigned long)s_gain_p1_prev_ok, pos_rate,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
            }
        }
        // else: 40-50% error — target range, no change needed

        // Log when high error was blocked by weak signal gate
        if (!changed && !can_reduce && err_rate > gain_err_high()
            && s_gain_idx > GAIN_FLOOR_IDX) {
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P1] Holding %.1f dB — err=%.0f%% but weak signals (ok=%lu, sig=%lu delta=%lu)\n",
                                 _ts, R820T_GAINS[s_gain_idx] / 10.0, err_rate * 100,
                                 (unsigned long)ok, (unsigned long)avg_signal, (unsigned long)avg_delta);
        }

        // Track throughput for next window's comparison
        s_gain_p1_prev_pos_rate = pos_rate;
        s_gain_p1_prev_ok = ok;

        if (changed) {
            s_gain_stable_count = 0;
        } else {
            s_gain_stable_count++;
            if (s_gain_stable_count >= GAIN_P1_STABLE_WIN) {
                // Converged — transition to phase 2
                s_gain_phase = 2;
                s_gain_err_ema = err_rate;
                s_gain_pos_rate_ema = pos_rate;
                s_gain_cooldown = 0;
                s_gain_step_dir = 0;
                s_gain_ceiling = R820T_GAIN_COUNT;
                s_gain_low_err_count = 0;
                s_gain_ceiling_strikes = 0;
                s_gain_ceiling_age = 0;
                s_gain_floor = -1;
                s_gain_high_err_count = 0;
                s_gain_floor_strikes = 0;
                s_gain_floor_age = 0;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: Converged at %.1f dB (err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu) → steady-state\n",
                                     _ts, R820T_GAINS[s_gain_idx] / 10.0, err_rate * 100, pos_rate, s_max_range_nm,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
            }
        }

    } else {
        // ══════════════════════════════════════════════════════════
        // Phase 2: Steady-state — throughput-optimized
        // Primary: did we get more valid position updates?
        // Emergency: error rate as clipping detector only
        // ══════════════════════════════════════════════════════════

        // EMA update — only when we have enough CRC-ok messages for reliable
        // metrics. During no-traffic periods (ok < 3, noise triggers only), the
        // EMAs freeze at their last known-good values rather than drifting toward
        // noise-floor error rates. This prevents the algorithm from thinking
        // error is 90%+ when traffic returns after a quiet period.
        if (ok >= GAIN_MIN_OK_FOR_STATS) {
            s_gain_err_ema = GAIN_P2_EMA_ALPHA * err_rate + (1.0f - GAIN_P2_EMA_ALPHA) * s_gain_err_ema;
            s_gain_pos_rate_ema = GAIN_P2_EMA_ALPHA * pos_rate + (1.0f - GAIN_P2_EMA_ALPHA) * s_gain_pos_rate_ema;
        }
        s_gain_error_rate = s_gain_err_ema;

        // ── Update historical profile (phase 2 only — 60s windows are reliable) ──
        // Gate on ok count: don't teach the profile "this gain produces nothing"
        // when there's simply no traffic.
        int gi = s_gain_idx;
        if (ok >= GAIN_MIN_OK_FOR_STATS && gi >= 0 && gi < (int)R820T_GAIN_COUNT) {
            if (s_gain_profile_samples[gi] == 0) {
                s_gain_profile[gi] = pos_rate; // seed
            } else {
                s_gain_profile[gi] = GAIN_PROFILE_ALPHA * pos_rate
                                   + (1.0f - GAIN_PROFILE_ALPHA) * s_gain_profile[gi];
            }
            if (s_gain_profile_samples[gi] < 65535)
                s_gain_profile_samples[gi]++;
        }

        // ── Emergency brake: severe error, BUT only if signals confirm clipping ──
        // High EMA error + strong signals = genuine ADC saturation → re-enter P1.
        // High EMA error + weak/absent signals = noise or no traffic → hold in P2.
        // Without this gate, traffic disappearing causes a false "clipping" diagnosis
        // that triggers P1 re-entry, oscillation, and the nighttime death spiral.
        if (s_gain_err_ema > GAIN_ERR_SEVERE) {
            bool evidence_of_clipping = (ok >= GAIN_MIN_OK_FOR_STATS
                                         && avg_signal >= GAIN_MIN_SIG_FOR_CLIPPING);
            if (evidence_of_clipping) {
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: Confirmed clipping (EMA err=%.0f%%, sig=%lu delta=%lu) → returning to fast convergence\n",
                                     _ts, s_gain_err_ema * 100,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
                s_gain_phase = 1;
                s_gain_stable_count = 0;
                s_gain_p1_prev_pos_rate = 0.0f;
                s_gain_p1_prev_ok = 0;
                s_gain_p1_last_was_up = false;
                s_gain_p1_up_suppress = 0;
                s_gain_ceiling = R820T_GAIN_COUNT;
                s_gain_low_err_count = 0;
                s_gain_ceiling_strikes = 0;
                s_gain_ceiling_age = 0;
                s_gain_floor = -1;
                s_gain_high_err_count = 0;
                s_gain_floor_strikes = 0;
                s_gain_floor_age = 0;
                s_gain_step_dir = 0;
                return;
            }
            // else: EMA err is high but signals are weak/absent — not clipping.
            // Don't log every window (too noisy) — the P2 heartbeat shows the state.
        }

        // ── Soft ceiling expiry: sustained low error OR hard time cap ──
        if (s_gain_ceiling < (int)R820T_GAIN_COUNT) {
            s_gain_ceiling_age++;

            // Hard time cap — always retry after 20 minutes regardless
            if (s_gain_ceiling_age >= GAIN_CEILING_MAX_WIN) {
                s_gain_ceiling = R820T_GAIN_COUNT;
                s_gain_low_err_count = 0;
                s_gain_ceiling_strikes = 0;
                s_gain_ceiling_age = 0;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: Ceiling expired (age=%d windows) → will retry higher\n",
                                     _ts, GAIN_CEILING_MAX_WIN);
            }
        }
        // Early-out: sustained low error clears ceiling sooner
        if (s_gain_err_ema < gain_err_very_low()) {
            s_gain_low_err_count++;
            if (s_gain_low_err_count >= GAIN_CEILING_CLEAR_WIN &&
                s_gain_ceiling < (int)R820T_GAIN_COUNT) {
                s_gain_ceiling = R820T_GAIN_COUNT;
                s_gain_low_err_count = 0;
                s_gain_ceiling_strikes = 0;
                s_gain_ceiling_age = 0;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: Ceiling cleared (EMA err=%.1f%% × %d windows) → will retry higher\n",
                                     _ts, s_gain_err_ema * 100, GAIN_CEILING_CLEAR_WIN);
            }
        } else {
            s_gain_low_err_count = 0;
        }

        // ── Soft floor expiry: sustained high error OR hard time cap ──
        if (s_gain_floor >= 0) {
            s_gain_floor_age++;

            // Hard time cap — always retry after 20 minutes regardless
            if (s_gain_floor_age >= GAIN_FLOOR_MAX_WIN) {
                s_gain_floor = -1;
                s_gain_high_err_count = 0;
                s_gain_floor_strikes = 0;
                s_gain_floor_age = 0;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: Floor expired (age=%d windows) → will retry lower\n",
                                     _ts, GAIN_FLOOR_MAX_WIN);
            }
        }
        // Early-out: sustained high error clears floor sooner (saturation is back)
        if (s_gain_err_ema > gain_err_very_high()) {
            s_gain_high_err_count++;
            if (s_gain_high_err_count >= GAIN_FLOOR_CLEAR_WIN &&
                s_gain_floor >= 0) {
                s_gain_floor = -1;
                s_gain_high_err_count = 0;
                s_gain_floor_strikes = 0;
                s_gain_floor_age = 0;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: Floor cleared (EMA err=%.1f%% × %d windows) → will retry lower\n",
                                     _ts, s_gain_err_ema * 100, GAIN_FLOOR_CLEAR_WIN);
            }
        } else {
            s_gain_high_err_count = 0;
        }

        // ── Cooldown after gain change ──
        if (s_gain_cooldown > 0) {
            s_gain_cooldown--;
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P2] Settling: %.1f dB, EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu (cooldown %d)\n",
                                 _ts, R820T_GAINS[s_gain_idx] / 10.0,
                                 s_gain_err_ema * 100, s_gain_pos_rate_ema,
                                 s_max_range_nm, (unsigned long)avg_signal, (unsigned long)avg_delta,
                                 s_gain_cooldown);
            return;
        }

        // ── Throughput comparison: evaluate result of last gain step ──
        if (s_gain_step_dir != 0 && s_gain_pre_step_pos_rate > 0.0f) {
            float change = (s_gain_pos_rate_ema - s_gain_pre_step_pos_rate) / s_gain_pre_step_pos_rate;

            if (change < -GAIN_P2_THROUGHPUT_DROP) {
                // Throughput dropped >15%. But was it the gain step, or traffic change?
                // Compare signal environment: if avg_signal shifted dramatically,
                // the traffic mix changed (aircraft arrived/departed) and the
                // throughput drop isn't attributable to the gain step.
                bool traffic_shifted = false;
                if (s_gain_pre_step_signal > 0 && ok >= GAIN_MIN_OK_FOR_STATS) {
                    float sig_change = fabsf((float)avg_signal - (float)s_gain_pre_step_signal)
                                       / (float)s_gain_pre_step_signal;
                    traffic_shifted = (sig_change > GAIN_P2_SIG_CHANGE_THRESH);
                }

                if (traffic_shifted) {
                    // Signal environment changed >40% — throughput drop is likely from
                    // traffic change, not the gain step. Hold current gain, no strike.
                    s_gain_step_dir = 0;
                    s_gain_cooldown = GAIN_P2_COOLDOWN;
                    char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                    serial_console_print("%sGAIN: [P2] Holding %.1f dB — throughput %+.0f%% but traffic shifted (sig %lu → %lu, no strike)\n",
                                         _ts, R820T_GAINS[s_gain_idx] / 10.0, change * 100,
                                         (unsigned long)s_gain_pre_step_signal, (unsigned long)avg_signal);
                    return;
                }

                // Signal environment is similar — throughput drop is real, revert
                int bad_idx = s_gain_idx;
                if (s_gain_step_dir > 0) {
                    // Stepped up and lost throughput → ceiling strike + step back
                    s_gain_ceiling_strikes++;
                    if (s_gain_ceiling_strikes >= GAIN_CEILING_STRIKES) {
                        s_gain_ceiling = s_gain_idx;
                        s_gain_ceiling_age = 0;
                    }
                    s_gain_idx--;
                } else {
                    // Stepped down and lost throughput → floor strike + step back up
                    s_gain_floor_strikes++;
                    if (s_gain_floor_strikes >= GAIN_FLOOR_STRIKES) {
                        s_gain_floor = s_gain_idx;
                        s_gain_floor_age = 0;
                    }
                    if (s_gain_idx < (int)R820T_GAIN_COUNT - 1) s_gain_idx++;
                }
                s_gain_cooldown = GAIN_P2_COOLDOWN;
                s_gain_step_dir = 0;
                gain_apply();
                // Build status suffix
                const char *sfx = "";
                if (s_gain_ceiling < (int)R820T_GAIN_COUNT) sfx = ", ceiling set";
                else if (s_gain_ceiling_strikes > 0) sfx = ", ceiling strike";
                else if (s_gain_floor >= 0) sfx = ", floor set";
                else if (s_gain_floor_strikes > 0) sfx = ", floor strike";
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P2] Reverted: %.1f → %.1f dB (throughput %+.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu%s)\n",
                                     _ts, R820T_GAINS[bad_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                     change * 100, s_gain_pos_rate_ema, s_max_range_nm,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta, sfx);
                return;
            } else {
                // Throughput held or improved — step was good, reset strikes
                s_gain_ceiling_strikes = 0;
                s_gain_floor_strikes = 0;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P2] Confirmed: %.1f dB (throughput %+.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu)\n",
                                     _ts, R820T_GAINS[s_gain_idx] / 10.0, change * 100, s_gain_pos_rate_ema, s_max_range_nm,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
                s_gain_step_dir = 0;
                // Let the new level settle before profile gets a vote
                s_gain_cooldown = GAIN_P2_COOLDOWN;
                return;
            }
        }

        // ── Decide whether to try a gain step ──
        // Check if historical profile suggests a better gain level nearby.
        // Require both 5% relative AND 0.5/s absolute improvement —
        // at low throughput (<10/s) the absolute floor prevents noise probes.
        int best_neighbor = -1;
        float best_rate = s_gain_pos_rate_ema;
        float prof_threshold = best_rate * 1.05f;
        if (prof_threshold < best_rate + GAIN_PROFILE_MIN_DELTA)
            prof_threshold = best_rate + GAIN_PROFILE_MIN_DELTA;

        // Look one step up (if below ceiling and not at max)
        if (s_gain_idx + 1 < (int)R820T_GAIN_COUNT && s_gain_idx + 1 < s_gain_ceiling) {
            int up = s_gain_idx + 1;
            if (s_gain_profile_samples[up] >= GAIN_PROFILE_MIN_WIN
                && s_gain_profile[up] > prof_threshold) {
                best_neighbor = up;
                best_rate = s_gain_profile[up];
            }
        }
        // Look one step down (if above hard floor and soft floor)
        if (s_gain_idx - 1 >= GAIN_FLOOR_IDX && s_gain_idx - 1 > s_gain_floor) {
            int dn = s_gain_idx - 1;
            if (s_gain_profile_samples[dn] >= GAIN_PROFILE_MIN_WIN
                && s_gain_profile[dn] > prof_threshold) {
                best_neighbor = dn;
                best_rate = s_gain_profile[dn];
            }
        }

        // If profile suggests a better neighbor, probe it
        if (best_neighbor >= 0) {
            s_gain_pre_step_pos_rate = s_gain_pos_rate_ema;
            s_gain_pre_step_signal = avg_signal;
            s_gain_step_dir = (best_neighbor > s_gain_idx) ? 1 : -1;
            s_gain_idx = best_neighbor;
            s_gain_cooldown = GAIN_P2_COOLDOWN;
            gain_apply();
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P2] Probing: %.1f → %.1f dB (profile: %.1f/s vs current %.1f/s, sig=%lu delta=%lu)\n",
                                 _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                 best_rate, s_gain_pos_rate_ema,
                                 (unsigned long)avg_signal, (unsigned long)avg_delta);
            return;
        }

        // No profile hint — use error rate heuristics for exploration
        if (s_gain_err_ema > gain_err_high() && s_gain_idx > GAIN_FLOOR_IDX
            && s_gain_idx - 1 > s_gain_floor
            && ok >= GAIN_MIN_OK_FOR_STATS && avg_signal >= GAIN_MIN_SIG_FOR_DOWN) {
            // Saturating (>50%) with strong signals — try stepping down
            s_gain_pre_step_pos_rate = s_gain_pos_rate_ema;
            s_gain_pre_step_signal = avg_signal;
            s_gain_step_dir = -1;
            s_gain_idx--;
            s_gain_cooldown = GAIN_P2_COOLDOWN;
            gain_apply();
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            serial_console_print("%sGAIN: [P2] Exploring down: %.1f → %.1f dB (EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu)\n",
                                 _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                 s_gain_err_ema * 100, s_gain_pos_rate_ema, s_max_range_nm,
                                 (unsigned long)avg_signal, (unsigned long)avg_delta);
        } else if (s_gain_err_ema < gain_err_low()
                   && s_gain_idx < (int)R820T_GAIN_COUNT - 1
                   && s_gain_idx + 1 < s_gain_ceiling) {
            // Below target band — graduated step-up based on how far below
            int raise_tenths;
            if (s_gain_err_ema <= 0.10f) {
                raise_tenths = GAIN_DB_RAISE_FULL;      // ≤10%: +3.0 dB
            } else if (s_gain_err_ema <= 0.20f) {
                raise_tenths = GAIN_DB_RAISE_STRONG;     // 10-20%: +2.5 dB
            } else if (s_gain_err_ema <= 0.30f) {
                raise_tenths = GAIN_DB_RAISE_MODERATE;   // 20-30%: +1.5 dB
            } else {
                raise_tenths = GAIN_DB_RAISE_MILD;       // 30-40%: +1.0 dB
            }
            int new_idx = gain_idx_for_db_raise(s_gain_idx, raise_tenths);
            // Clamp to ceiling
            if (new_idx >= s_gain_ceiling) new_idx = s_gain_ceiling - 1;
            if (new_idx <= s_gain_idx) new_idx = s_gain_idx + 1; // always move at least 1
            if (new_idx < (int)R820T_GAIN_COUNT && new_idx < s_gain_ceiling) {
                s_gain_pre_step_pos_rate = s_gain_pos_rate_ema;
                s_gain_pre_step_signal = avg_signal;
                s_gain_step_dir = 1;
                s_gain_idx = new_idx;
                s_gain_cooldown = GAIN_P2_COOLDOWN;
                gain_apply();
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [P2] Exploring up: %.1f → %.1f dB (+%.1f dB, EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu)\n",
                                     _ts, R820T_GAINS[old_idx] / 10.0, R820T_GAINS[s_gain_idx] / 10.0,
                                     (R820T_GAINS[s_gain_idx] - R820T_GAINS[old_idx]) / 10.0,
                                     s_gain_err_ema * 100, s_gain_pos_rate_ema, s_max_range_nm,
                                     (unsigned long)avg_signal, (unsigned long)avg_delta);
            }
        }

        // ── P2 heartbeat: always log current state for webapp visibility ──
        {
            extern device_settings_t g_settings;
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            const char *bounds = "";
            if (s_gain_ceiling < (int)R820T_GAIN_COUNT && s_gain_floor >= 0)
                bounds = ", bounds: ceiling+floor set";
            else if (s_gain_ceiling < (int)R820T_GAIN_COUNT)
                bounds = ", bounds: ceiling set";
            else if (s_gain_floor >= 0)
                bounds = ", bounds: floor set";
            serial_console_print("%sGAIN: [P2] Steady: %.1f dB, EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu, band=%d-%d%%%s\n",
                                 _ts, R820T_GAINS[s_gain_idx] / 10.0,
                                 s_gain_err_ema * 100, s_gain_pos_rate_ema,
                                 s_max_range_nm,
                                 (unsigned long)avg_signal, (unsigned long)avg_delta,
                                 g_settings.gain_err_low, g_settings.gain_err_high,
                                 bounds);
        }
    }
}

// ============================================================
// Gain from levels and yield (2.4 Msps demodulator) – see gain_ctl.h
// ============================================================

// What the decoder counted since the window was opened, and a fresh window.
static void gain_window_take(mode_s_t *st, float secs, gain_obs_t *o)
{
    mode_s_level_t lv = {0};
    uint32_t ok = st->stat_crc_ok;
    memset(o, 0, sizeof(*o));
    o->secs = secs;
    o->ok = ok;
    o->pos = s_gain_win_pos;
    o->loud = st->stat_loud;
    o->es_ok = st->stat_es_ok;
    o->es_lost = st->stat_es_lost;
    o->avg_signal = ok ? (uint32_t)(st->stat_signal_sum / ok) : 0;
    o->avg_delta = ok ? (uint32_t)(st->stat_delta_sum / ok) : 0;
    o->range_nm = (float)s_max_range_nm;
    if (st->lvl_hist) {
        mode_s_level_read(st->lvl_hist, &lv);
        memset(st->lvl_hist, 0, MODE_S_LVL_BINS * sizeof(uint32_t));
    }
    o->noise = lv.noise;
    o->clip = lv.clip;
    st->stat_crc_ok = 0;
    st->stat_crc_fail = 0;
    st->stat_preambles = 0;
    st->stat_signal_sum = 0;
    st->stat_delta_sum = 0;
    st->stat_loud = 0;
    st->stat_es_ok = 0;
    st->stat_es_lost = 0;
    s_gain_win_pos = 0;
}

static void gain_levels_publish(const gain_obs_t *o)
{
    s_gain_noise = o->noise;
    s_gain_headroom = gain_ctl_headroom_db(o->noise);
    s_gain_loud = o->ok >= GAIN_LOUD_MIN_OK ? (float)o->loud / (float)o->ok : -1.0f;
    s_gain_clip = o->clip;
    s_gain_avg_signal = o->avg_signal;
    s_gain_avg_delta = o->avg_delta;
}

// The level figures as text, for the lines this file writes itself.
static void gain_levels_text(const gain_obs_t *o, char *buf, size_t n)
{
    char loud[16];
    if (o->ok >= GAIN_LOUD_MIN_OK) snprintf(loud, sizeof(loud), "%.0f%%", 100.0 * o->loud / o->ok);
    else snprintf(loud, sizeof(loud), "n/a");
    if (o->noise > 0)
        snprintf(buf, n, "loud=%s noise=%.1f headroom=%.0fdB clip=%.2f%%", loud, (double)o->noise,
                 (double)gain_ctl_headroom_db(o->noise), (double)(o->clip * 100.0f));
    else
        snprintf(buf, n, "loud=%s noise=n/a clip=%.2f%%", loud, (double)(o->clip * 100.0f));
}

// Lines from gain_ctl.c, each given the time stamp and queued for the log
// task: the decoder does not wait for the console (docs/UPLINK.md 2.2).
static void gain_log_lines(char *text)
{
    char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
    for (char *line = text; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        adsb_logq_printf("%s%s\n", _ts, line);
        line = nl ? nl + 1 : NULL;
    }
}

// A new gain: tell the tuner, and open no window until the samples taken at
// the old gain have left the ring and the tuner has settled.
static void gain_level_set(int idx, int64_t now)
{
    s_gain_idx = idx;
    gain_apply();
    s_gl_settle_until = now + GAIN_SETTLE_US;
}

// A sweep ends: back to the gain it started from (unless the gain has been
// set by hand in the meantime). The adaptive loop, if it is on, carries on
// from there with what it knew.
static void gain_sweep_end(int64_t now, const char *how, bool restore)
{
    s_sweep.active = false;
    s_sweep_running = false;
    if (restore) {
        gain_level_set(s_sweep.saved_idx, now);
        if (s_gl) s_gl->ctl.idx = s_sweep.saved_idx;
    } else {
        s_gl_settle_until = now + GAIN_SETTLE_US;
    }
    char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
    adsb_logq_printf("%sGAIN: [Sweep] %s; now at %.1f dB (%s)\n", _ts, how,
                     R820T_GAINS[s_gain_idx] / 10.0, s_gain_auto ? "adaptive" : "manual");
}

// One step of a sweep. Returns true while the sweep owns the gain.
static bool gain_sweep_tick(mode_s_t *st, int64_t now)
{
    int32_t req = s_sweep_req;
    if (req != 0) {
        s_sweep_req = 0;
        if (req > 0 && !s_sweep.active) {
            s_sweep.active = true;
            s_sweep_running = true;
            s_sweep.saved_idx = s_gain_idx;
            s_sweep.idx = (int)R820T_GAIN_COUNT - 1;
            s_sweep.dwell_us = (int64_t)req * 1000000LL;
            gain_level_set(s_sweep.idx, now);
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            adsb_logq_printf("%sGAIN: [Sweep] %d steps from %.1f down to %.1f dB, %ld s each; `gain sweep stop` ends it\n",
                             _ts, (int)R820T_GAIN_COUNT - GAIN_FLOOR_IDX, R820T_GAINS[R820T_GAIN_COUNT - 1] / 10.0,
                             R820T_GAINS[GAIN_FLOOR_IDX] / 10.0, (long)req);
            return true;
        }
        if (req < 0 && s_sweep.active) {
            gain_sweep_end(now, "stopped", req == -1);
            return true;
        }
    }
    if (!s_sweep.active) return false;

    if (s_gl_settle_until) {
        if (now < s_gl_settle_until) return true;
        gain_obs_t drop;
        s_gl_settle_until = 0;
        gain_window_take(st, 1.0f, &drop);   // a fresh window at the new gain
        s_sweep.t0 = now;
        return true;
    }
    if (now - s_sweep.t0 < s_sweep.dwell_us) return true;

    {
        gain_obs_t o;
        char lv[96];
        float secs = (float)(now - s_sweep.t0) / 1e6f;
        gain_window_take(st, secs, &o);
        gain_levels_publish(&o);
        gain_levels_text(&o, lv, sizeof(lv));
        float err = gain_ctl_err(&o);
        char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
        adsb_logq_printf("%sGAIN: [Sweep] %.1f dB: %.1f msg/s, ES %.1f/s, pos=%.1f/s, err=%.0f%%, sig=%lu delta=%lu, %s\n",
                         _ts, R820T_GAINS[s_sweep.idx] / 10.0, (double)(o.ok / secs), (double)(o.es_ok / secs),
                         (double)(o.pos / secs), (double)(err < 0 ? 0.0f : err * 100.0f),
                         (unsigned long)o.avg_signal, (unsigned long)o.avg_delta, lv);
    }
    if (s_sweep.idx > GAIN_FLOOR_IDX) {
        s_sweep.idx--;
        gain_level_set(s_sweep.idx, now);
        return true;
    }
    gain_sweep_end(now, "done", true);
    return true;
}

// Called by the decoder task after every run of the decoder.
static void gain_level_tick(mode_s_t *st)
{
    int64_t now = esp_timer_get_time();
    if (s_gl == NULL) return;
    gain_ctl_t *g = &s_gl->ctl;

    uint32_t req = s_gl_req;
    if (req) {
        s_gl_req = 0;
        gain_ctl_restart(g, req == GL_REQ_RESTART_MAX);
        if (req == GL_REQ_RESTART_KEEP) g->idx = s_gain_idx;
        s_gain_phase = 1;
        s_gl_settle_until = now + GAIN_SETTLE_US;   // whoever asked has set the gain as well
    }

    if (gain_sweep_tick(st, now)) {
        s_gain_last_eval = now;
        return;
    }

    if (s_gl_settle_until) {
        if (now < s_gl_settle_until) return;
        gain_obs_t drop;
        s_gl_settle_until = 0;
        gain_window_take(st, 1.0f, &drop);
        s_gain_last_eval = now;
        return;
    }

    int64_t elapsed = now - s_gain_last_eval;

    if (!s_gain_auto) {
        // Fixed gain: the same figures once a minute, for whoever watches.
        if (elapsed < GAIN_P2_WINDOW_US) return;
        gain_obs_t o;
        char lv[96];
        float secs = (float)elapsed / 1e6f;
        gain_window_take(st, secs, &o);
        gain_levels_publish(&o);
        gain_levels_text(&o, lv, sizeof(lv));
        float err = gain_ctl_err(&o);
        if (err >= 0) s_gain_error_rate = err;
        s_gain_pos_rate_ema = o.pos / secs;
        char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
        adsb_logq_printf("%sGAIN: [Manual] %.1f dB, err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu, %s\n",
                         _ts, R820T_GAINS[s_gain_idx] / 10.0, (double)(err < 0 ? 0.0f : err * 100.0f),
                         (double)(o.pos / secs), s_max_range_nm,
                         (unsigned long)o.avg_signal, (unsigned long)o.avg_delta, lv);
        s_gain_last_eval = now;
        return;
    }

    if (s_gain_warmup) {
        // Let the decoder run for a while before the first window.
        if (elapsed < GAIN_WARMUP_US) return;
        gain_obs_t drop;
        s_gain_warmup = false;
        gain_window_take(st, 1.0f, &drop);
        s_gain_last_eval = now;
        char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
        adsb_logq_printf("%sGAIN: Warmup complete — starting adaptive gain (levels and yield)\n", _ts);
        return;
    }

    // A window ends when its time is up; in phase 1 also once enough frames
    // are DECODED (not: candidates seen) and it is at least two seconds old.
    bool due = elapsed >= (int64_t)gain_ctl_window_us(g) ||
               (g->phase == 1 && elapsed >= (int64_t)GAIN_P1_MIN_US && st->stat_crc_ok >= GAIN_P1_MSGS);
    if (!due) return;

    gain_obs_t o;
    char text[640];
    int old_idx = g->idx;
    gain_window_take(st, (float)elapsed / 1e6f, &o);
    gain_levels_publish(&o);
    int idx = gain_ctl_window(g, &o, text, sizeof(text));
    s_gain_phase = g->phase;
    s_gain_error_rate = g->err_last;
    s_gain_pos_rate_ema = g->pos_last;
    gain_log_lines(text);
    s_gain_last_eval = now;
    if (idx != old_idx) gain_level_set(idx, now);
}

// --- Sort state for on-device aircraft list ---
static adsb_sort_col_t s_sort_col = ADSB_SORT_DIST;
static bool s_sort_asc = true;

void adsb_set_sort(int col, bool ascending) {
    s_sort_col = (adsb_sort_col_t)col;
    s_sort_asc = ascending;
}

// One row of the list as copied out of the aircraft table.
typedef struct {
    uint32_t icao;
    char callsign[9];
    uint8_t has_position;
    int altitude, speed, heading;
    double lat, lon;
    float dist_km;
} list_row_t;

// The copy is in PSRAM and belongs to the one caller (the ADS-B app task).
static EXT_RAM_BSS_ATTR list_row_t s_list_rows[AIRCRAFT_TABLE_SIZE];

static int list_row_cmp(const list_row_t *a, const list_row_t *b, adsb_sort_col_t col)
{
    switch (col) {
        case ADSB_SORT_ICAO: return (int)(a->icao > b->icao) - (int)(a->icao < b->icao);
        case ADSB_SORT_CALL: return strcmp(a->callsign, b->callsign);
        case ADSB_SORT_ALT:  return (a->altitude > b->altitude) - (a->altitude < b->altitude);
        case ADSB_SORT_SPD:  return (a->speed > b->speed) - (a->speed < b->speed);
        case ADSB_SORT_HDG:  return (a->heading > b->heading) - (a->heading < b->heading);
        case ADSB_SORT_DIST:
        default:             return (a->dist_km > b->dist_km) - (a->dist_km < b->dist_km);
    }
}

// The aircraft list as text, one row per aircraft, sorted.
//
// The aircraft table is only copied under its lock; distances, the sort and
// the text are done after it is released, in single precision and without
// printf's "%f". Up to coh3 all of it ran inside the lock, with a
// double-precision haversine per aircraft: 4.2 million instructions for 100
// aircraft, during which the decoder could not deliver a message
// (docs/UPLINK.md section 9.7).
//
// One caller at a time: the copy is a static buffer.
int adsb_format_aircraft_list(char *buf, int bufsize) {
    int pos = 0;
    int64_t now = esp_timer_get_time();
    receiver_pos_t rx = adsb_get_receiver_pos();

    if (!aircraft_mutex) {
        pos += snprintf(buf + pos, bufsize - pos, "Not initialized");
        return pos;
    }

    int count = 0;
    xSemaphoreTake(aircraft_mutex, portMAX_DELAY);
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
        const aircraft_t *a = &aircraft_table[i];
        if (!a->active) continue;
        if ((now - a->last_seen) > AIRCRAFT_MAX_AGE_US) continue;
        list_row_t *r = &s_list_rows[count++];
        r->icao = a->icao;
        memcpy(r->callsign, a->callsign, sizeof(r->callsign));
        r->has_position = a->has_position ? 1 : 0;
        r->altitude = a->altitude;
        r->speed = a->speed;
        r->heading = a->heading;
        r->lat = a->lat;
        r->lon = a->lon;
    }
    xSemaphoreGive(aircraft_mutex);

    // Distances
    for (int i = 0; i < count; i++) {
        list_row_t *r = &s_list_rows[i];
        r->callsign[8] = '\0';
        r->dist_km = 1e9f;   // no position: sorts last by distance
        if (rx.fix_valid && r->has_position) {
            float brg;
            adsb_gc(rx.lat, rx.lon, r->lat, r->lon, &r->dist_km, &brg);
        }
    }

    // Insertion sort of an index by the selected column (stable, as before)
    const adsb_sort_col_t col = s_sort_col;
    const bool asc = s_sort_asc;
    uint16_t order[AIRCRAFT_TABLE_SIZE];
    for (int i = 0; i < count; i++) {
        int j = i - 1;
        while (j >= 0) {
            int cmp = list_row_cmp(&s_list_rows[order[j]], &s_list_rows[i], col);
            if (!asc) cmp = -cmp;
            if (cmp <= 0) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = (uint16_t)i;
    }

    if (count == 0) {
        pos += snprintf(buf + pos, bufsize - pos, "No aircraft");
    }

    for (int e = 0; e < count && pos < bufsize - 80; e++) {
        const list_row_t *ac = &s_list_rows[order[e]];
        char call[9] = "--------";
        if (ac->callsign[0]) {
            snprintf(call, sizeof(call), "%-8s", ac->callsign);
        }

        char dist_str[12] = "   --";
        if (rx.fix_valid && ac->has_position && ac->dist_km < 1e8f) {
            adsb_put_fixed_f(dist_str, sizeof(dist_str), ac->dist_km * 0.539957f, 1, 5, ' ');
        }

        char alt_str[12] = "  ----";
        if (ac->altitude != 0) {
            snprintf(alt_str, sizeof(alt_str), "%6d", ac->altitude);
        }

        char spd_str[12] = " ---";
        if (ac->speed > 0) {
            snprintf(spd_str, sizeof(spd_str), "%4d", ac->speed);
        }

        char hdg_str[12] = "---";
        if (ac->heading > 0) {
            snprintf(hdg_str, sizeof(hdg_str), "%3d", (int)ac->heading);
        }

        pos += snprintf(buf + pos, bufsize - pos,
            "%06lX %-8s%s %s %s %s\n",
            (unsigned long)ac->icao, call, alt_str, spd_str, hdg_str, dist_str);
    }

    return count;
}

// Great-circle distance and bearing: adsb_gc() in adsb_fmt.c (single
// precision). The double-precision haversine() that was here cost about
// 38,000 instructions a call on this processor.

#include "sd_logger.h"

// ============================================================
// SD card logging
// ============================================================

static sd_log_ch_t sd_ch;                    // shared channel handle
static int64_t sd_log_last_sync = 0;
#define SD_SYNC_INTERVAL_US  1000000LL       // flush every 1s if ≥1 sector
#define SD_SYNC_MAX_US       5000000LL       // hard max: flush every 5s regardless
#define SD_SYNC_MIN_BYTES    512             // min data for time-based flush

// PSRAM write buffer — messages accumulate here between flushes.
// File handle stays OPEN between sync cycles for performance.
// fsync() on every flush ensures data is committed to the card,
// so a hard reset loses at most one sync interval of buffered data.
#define SD_LOG_BUFSIZE  (64 * 1024)
static char *sd_log_buf = NULL;
static int   sd_log_buf_pos = 0;

static char sd_log_filename_buf[64];  // scratch for filename generation

static const char *sd_log_pick_filename(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo);

    if (!time_manager_is_trusted_fn() || now < 1704067200LL) {
        // No GPS/NTP fix yet — use boot-numbered name.
        // Will be renamed to UTC timestamp when trusted time arrives.
        snprintf(sd_log_filename_buf, sizeof(sd_log_filename_buf),
            "/sdcard/logs/adsb_boot%s.csv", sd_config_boot_id());
    } else {
        strftime(sd_log_filename_buf, sizeof(sd_log_filename_buf),
            "/sdcard/logs/adsb_%Y-%m-%dT%H%M%SZ.csv", &timeinfo);
    }
    return sd_log_filename_buf;
}

#define ADSB_CSV_HEADER \
    "timestamp_utc,raw_msg,icao,callsign,altitude_ft,speed_kt," \
    "heading_deg,vrate_fpm,lat,lon,squawk," \
    "rx_lat,rx_lon,range_km,bearing_deg," \
    "rx_sats,rx_hdop\n"

static void sd_log_create(void) {
    const char *fn = sd_log_pick_filename();
    if (!sd_log_ch_open(&sd_ch, fn, ADSB_CSV_HEADER)) return;
    sd_log_last_sync = esp_timer_get_time();
    // NOTE: sd_log_buf_pos intentionally NOT zeroed — the buffer may
    // contain valid CSV rows from before a failure.  The caller flushes
    // the buffer after create/reopen.
}

static void sd_log_flush(void) {
    if (!sd_ch.initialized || sd_log_buf_pos == 0) return;
    int n = sd_log_buf_pos;
    sd_log_ch_flush(&sd_ch, sd_log_buf, n);
    if (sd_ch.initialized) {
        // Flush succeeded — clear buffer
        sd_log_buf_pos = 0;
        sd_log_last_sync = esp_timer_get_time();
        sd_log_record_flush(SD_FLUSH_CH_ADSB);
    }
}

// Close the log permanently (for clean shutdown/unmount).
void sd_log_close(void) {
    if (!sd_ch.initialized) return;
    sd_log_flush();
    sd_log_ch_close(&sd_ch);
}

// Print current SD log status.
void sd_log_print_status(void) {
    if (sd_ch.initialized && sd_ch.filename[0]) {
        printf("I (%lu) CLASS: SD log: %s\n",
            (unsigned long)(esp_timer_get_time() / 1000), sd_ch.filename);
    }
}

// Check if ADS-B SD logging is active (log file open and header written).
bool sd_log_is_active(void) {
    return sd_ch.initialized;
}

// Close current log and create a fresh one with a new boot-numbered filename.
// Called after MSC mode exit to start a clean log file.
void sd_log_create_new(void) {
    if (sd_ch.initialized) sd_log_close();
    sd_log_create();
}

// Rename boot-numbered log to UTC-timestamped name.
// Called once from GPS task when the system clock is first set.
void sd_log_rename_with_time(void) {
    if (!sd_ch.initialized) return;
    if (strstr(sd_ch.filename, "adsb_boot") == NULL) return;
    if (!time_manager_is_trusted_fn()) return;  // wait for GPS/NTP

    ESP_LOGI(TAG, "SD rename starting [DMA free=%u largest=%u, internal=%u]",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    struct tm timeinfo;
    time_t now;
    time(&now);
    gmtime_r(&now, &timeinfo);

    char new_filename[64];
    strftime(new_filename, sizeof(new_filename),
        "/sdcard/logs/adsb_%Y-%m-%dT%H%M%SZ.csv", &timeinfo);

    if (sd_log_ch_rename(&sd_ch, new_filename, sd_log_buf, sd_log_buf_pos)) {
        sd_log_buf_pos = 0;  // buffer was flushed by rename
    } else {
        // Rename failed — channel is now uninitialized.  Create a fresh
        // file (sd_log_create picks a new timestamp name + writes header).
        // Buffer was already flushed by rename before the failure.
        sd_log_buf_pos = 0;
        sd_log_create();
    }
}

// Append a formatted timestamp to the PSRAM buffer.
static void sd_log_buf_timestamp(void) {
    if (!sd_log_buf) return;
    int avail = SD_LOG_BUFSIZE - sd_log_buf_pos;
    if (avail < 40) return;  // need room for timestamp

    int n;
    if (time_manager_is_trusted_fn()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm_info;
        gmtime_r(&tv.tv_sec, &tm_info);
        n = snprintf(sd_log_buf + sd_log_buf_pos, avail,
            "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
            tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
            tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
            (long)(tv.tv_usec / 1000));
    } else {
        int64_t boot_ms = esp_timer_get_time() / 1000;
        n = snprintf(sd_log_buf + sd_log_buf_pos, avail,
            "boot+%lld.%03lld",
            (long long)(boot_ms / 1000), (long long)(boot_ms % 1000));
    }
    if (n > 0) sd_log_buf_pos += n;
}

// One CSV row. Called by the decoder task after it has released the
// aircraft lock, with its own copy of the aircraft and the receiver position
// and range it has already worked out for the console line. The text is the
// same as before; the numbers are written without printf's "%f"
// (adsb_fmt.h), which was most of the cost of a row.
static void sd_log_aircraft(const aircraft_t *ac, const struct mode_s_msg *mm,
                            const receiver_pos_t *rxp, float range_km, float bearing) {
    // Don't write to SD while USB MSC owns the card
    extern bool sd_msc_is_active_fn(void);
    if (sd_msc_is_active_fn()) return;

    // Allocate buffer once; init channel on first call
    if (!sd_log_buf) {
        sd_log_buf = heap_caps_malloc(SD_LOG_BUFSIZE, MALLOC_CAP_SPIRAM);
        if (!sd_log_buf) return;
        sd_log_buf_pos = 0;
        sd_log_ch_init(&sd_ch, TAG);
    }

    // Lazy create: retry if the log hasn't been created yet
    if (!sd_ch.initialized) {
        // If the FAT is structurally corrupt, stop retrying entirely.
        // User must reformat the card and run 'mount'.
        if (sd_log_bus_failed()) return;

        static int64_t last_retry = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_retry < 5000000LL) return;  // 5s throttle
        last_retry = now_us;

        extern bool sd_is_mounted(void);

        // 1. If we have a previous filename and card is mounted, try reopen
        if (sd_ch.filename[0] && sd_is_mounted()) {
            sd_log_ch_reopen(&sd_ch);
            if (sd_ch.initialized && sd_log_buf_pos > 0) {
                sd_log_flush();  // flush any buffered data from before failure
            }
        }

        // 2. Card mounted but reopen failed (or no previous file) → new file
        if (!sd_ch.initialized && sd_is_mounted()) {
            sd_log_create();
            if (sd_ch.initialized && sd_log_buf_pos > 0) {
                sd_log_flush();  // flush old buffer to new file
            }
        }

        // 3. Card not mounted → try recovery, then reopen or create
        if (!sd_ch.initialized) {
            if (sd_log_try_recovery(TAG)) {
                // Recovery succeeded — try reopen original file
                if (sd_ch.filename[0]) {
                    sd_log_ch_reopen(&sd_ch);
                }
                // If reopen failed, create new file
                if (!sd_ch.initialized) {
                    sd_log_create();
                }
                if (sd_ch.initialized && sd_log_buf_pos > 0) {
                    sd_log_flush();
                }
            }
        }

        // 4. Card is mounted but we still can't create files → FAT is
        //    structurally corrupt (zombie card).  Stop retrying to avoid
        //    an infinite 5s loop of failed fopen() calls.
        if (!sd_ch.initialized && sd_is_mounted()) {
            sd_log_bus_fail("Card mounted but cannot create files "
                            "— FAT corrupted, reformat card and run 'mount'");
        }

        if (!sd_ch.initialized) return;
    }

    // Check buffer space — flush if getting full (leave 512B headroom)
    if (sd_log_buf_pos > SD_LOG_BUFSIZE - 512) {
        sd_log_flush();
    }

    const receiver_pos_t rx = *rxp;

    // Write to PSRAM buffer (not to disk)
    sd_log_buf_timestamp();

    int avail = SD_LOG_BUFSIZE - sd_log_buf_pos;
    if (avail < 256) return;  // safety margin

    // Raw hex message
    int n = snprintf(sd_log_buf + sd_log_buf_pos, avail, ",");
    if (n > 0) sd_log_buf_pos += n;

    if (mm) {
        int msglen = mm->msgbits / 8;
        if (msglen > 14) msglen = 14;
        sd_log_buf_pos += adsb_put_hex(sd_log_buf + sd_log_buf_pos, SD_LOG_BUFSIZE - sd_log_buf_pos,
                                       mm->msg, msglen);
    }

    // ",ICAO,callsign,alt,speed,heading,vrate,lat,lon,squawk,rxlat,rxlon,range_km,bearing,sats,hdop"
    {
        char *b = sd_log_buf + sd_log_buf_pos;
        int left = SD_LOG_BUFSIZE - sd_log_buf_pos, w = 0;
#define SD_PUT(expr) do { int k_ = (expr); if (k_ > 0 && k_ < left - w) w += k_; } while (0)
        SD_PUT(snprintf(b + w, left - w, ",%06lX,%s,%d,%d,%d,%d,",
                        (unsigned long)ac->icao, ac->callsign[0] ? ac->callsign : "",
                        ac->altitude, ac->speed, ac->heading, ac->vert_rate));
        SD_PUT(adsb_put_fixed(b + w, left - w, ac->lat, 5, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ","));
        SD_PUT(adsb_put_fixed(b + w, left - w, ac->lon, 5, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ",%04d,", ac->squawk));
        SD_PUT(adsb_put_fixed(b + w, left - w, rx.fix_valid ? rx.lat : 0.0, 5, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ","));
        SD_PUT(adsb_put_fixed(b + w, left - w, rx.fix_valid ? rx.lon : 0.0, 5, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ","));
        SD_PUT(adsb_put_fixed_f(b + w, left - w, range_km, 1, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ","));
        SD_PUT(adsb_put_fixed_f(b + w, left - w, bearing, 0, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ",%d,", rx.sats));
        SD_PUT(adsb_put_fixed(b + w, left - w, rx.hdop, 1, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, "\n"));
#undef SD_PUT
        sd_log_buf_pos += w;
    }

    // Periodic flush:
    //   • Buffer nearly full → immediate (safety)
    //   • ≥1s since last flush AND ≥512 bytes → flush (one full sector min)
    //   • ≥5s since last flush → flush regardless of size (low-traffic fallback)
    int64_t sync_now = esp_timer_get_time();
    int64_t since_self = sync_now - sd_log_last_sync;
    bool buf_critical = sd_log_buf_pos > SD_LOG_BUFSIZE - 512;
    bool has_block = sd_log_buf_pos >= SD_SYNC_MIN_BYTES;

    bool should_flush = false;
    if (buf_critical) {
        should_flush = true;
    } else if (since_self >= SD_SYNC_MAX_US && sd_log_buf_pos > 0) {
        should_flush = true;                                          // 5s hard max
    } else if (since_self >= SD_SYNC_INTERVAL_US && has_block) {
        should_flush = true;                                          // 1s + ≥512B
    }

    if (should_flush) {
        sd_log_flush();
    }
}
// ============================================================

// ============================================================
// LVGL display
// ============================================================

void adsb_update_display(lv_obj_t *table) {
    // LVGL table cell updates disabled — lv_table_set_cell_value calls realloc
    // which crashes on heap metadata corrupted by USB DMA.  Table shows headers
    // only.  Aircraft data is logged to SD card via sd_log_aircraft().
    (void)table;
    return;
}

// ============================================================
// on_msg
// ============================================================

void on_msg(mode_s_t *self, struct mode_s_msg *mm)
{
    // mm is already fully decoded by mode_s_detect() → mode_s_decode()
    // before invoking this callback.  Do NOT re-decode: mode_s_decode()
    // does memset(mm,0,...) which zeroes mm->msg, and since the third
    // arg (msg) points into the same struct, the copy-back reads zeros.

    uint32_t icao = (uint32_t)(mm->aa1 << 16 | mm->aa2 << 8 | mm->aa3);
    if (icao == 0) return;

    // A recording is being replayed: its messages are counted, tracked and
    // shown, but go no further (no log line, SD log or MQTT).
    const bool replayed = s_replay_counting;
    if (replayed) s_replay_msgs++;

    if (!aircraft_mutex) return;
    xSemaphoreTake(aircraft_mutex, portMAX_DELAY);
    aircraft_t *ac = aircraft_get(icao);
    ac->last_seen = esp_timer_get_time();
    ac->msg_count++;
    const bool first_msg = (ac->msg_count == 1);

    // --- Update aircraft state from this message ---

    // Callsign (DF17, ME 1-4)
    if (mm->msgtype == 17 && mm->metype >= 1 && mm->metype <= 4) {
        strncpy(ac->callsign, mm->flight, 8);
        ac->callsign[8] = '\0';
        for (int i = 7; i >= 0 && ac->callsign[i] == ' '; i--)
            ac->callsign[i] = '\0';
    }

    // Altitude — only from message types that carry it:
    //   DF0/4/16/20: 13-bit AC field (decode_ac13_field)
    //   DF17 TC 9-18: 12-bit altitude in airborne position (decode_ac12_field)
    if (mm->msgtype == 0 || mm->msgtype == 4 ||
        mm->msgtype == 16 || mm->msgtype == 20) {
        if (mm->altitude) ac->altitude = mm->altitude;
    } else if (mm->msgtype == 17 && mm->metype >= 9 && mm->metype <= 18) {
        if (mm->altitude) ac->altitude = mm->altitude;
    }

    // CPR position (DF17, TC 9-18)
    if (mm->msgtype == 17 && mm->metype >= 9 && mm->metype <= 18) {
        cpr_cache_t *c = cpr_get(icao);
        int64_t now = esp_timer_get_time();
        if (mm->fflag) {
            c->odd_lat   = mm->raw_latitude;
            c->odd_lon   = mm->raw_longitude;
            c->odd_ts    = now;
            c->odd_valid = 1;
        } else {
            c->even_lat   = mm->raw_latitude;
            c->even_lon   = mm->raw_longitude;
            c->even_ts    = now;
            c->even_valid = 1;
        }

        int decoded = 0;

        // ── CPR decode strategy with position jump detection:
        //
        //    Every candidate position (local or global) is jump-checked against
        //    the last known good position (good_lat/good_lon). Implausible jumps
        //    trigger suspect mode, which suppresses local decode (poisoned reference)
        //    and waits for a clean global decode to recover.
        //
        //    Jump threshold: max(10nm, elapsed_seconds × 1.0 nm/s) — ~3600 kt,
        //    faster than any transponder-equipped aircraft including hypersonic.
        //    ──

        #define CPR_TIGHT_AGE_US  2000000LL  // 2 seconds — tight global is trustworthy

        int64_t frame_age = c->even_ts - c->odd_ts;
        if (frame_age < 0) frame_age = -frame_age;

        // 1. Local decode: primary path (skip if suspect — reference is poisoned)
        if (ac->has_position && !ac->position_suspect) {
            double new_lat, new_lon;
            if (cpr_decode_local(ac->lat, ac->lon,
                                 mm->raw_latitude, mm->raw_longitude,
                                 mm->fflag, &new_lat, &new_lon)) {
                if (cpr_jump_check(ac, new_lat, new_lon, now)) {
                    // Good local decode — accept
                    ac->lat = new_lat;
                    ac->lon = new_lon;
                    c->lat = new_lat;
                    c->lon = new_lon;
                    ac->good_lat = new_lat;
                    ac->good_lon = new_lon;
                    ac->good_pos_ts = now;
                    ac->suspect_count = 0;
                    decoded = 1;
                } else {
                    // Local jumped — reference is poisoned, enter suspect mode
                    ac->position_suspect = 1;
                    ac->suspect_count = 1;
                    ac->suspect_agree = 0;
                    ac->suspect_lat = 0;
                    ac->suspect_lon = 0;
                    adsb_logq_printf("CPR: %06lX position suspect — local jumped, awaiting global recovery\n",
                                         ac->icao);
                    // Fall through to global decode for immediate recovery attempt
                }
            }
        }

        // 2. Global decode
        if (c->even_valid && c->odd_valid && frame_age <= CPR_MAX_AGE_US) {
            if (cpr_decode(c)) {
                double gLat = c->lat, gLon = c->lon;

                if (!ac->has_position) {
                    // First contact — no good_pos, jump check auto-passes
                    ac->lat = gLat;
                    ac->lon = gLon;
                    ac->has_position = 1;
                    ac->good_lat = gLat;
                    ac->good_lon = gLon;
                    ac->good_pos_ts = now;
                    decoded = 1;

                } else if (ac->position_suspect) {
                    // Recovery mode — two paths to accept:
                    //   A. Global passes jump check against good_pos (good_pos was right)
                    //   B. N consecutive globals agree with each other (good_pos was wrong)

                    int recovered = 0;
                    int via_consensus = 0;

                    // Path A: matches good_pos — immediate recovery
                    if (cpr_jump_check(ac, gLat, gLon, now)) {
                        recovered = 1;
                    }

                    // Path B: consensus — globals agreeing with each other
                    if (!recovered) {
                        if (ac->suspect_agree > 0) {
                            // Check against previous candidate
                            double dlat = gLat - ac->suspect_lat;
                            double dlon = (gLon - ac->suspect_lon) * cos(ac->suspect_lat * M_PI / 180.0);
                            double dist_nm = sqrt(dlat * dlat + dlon * dlon) * 60.0;
                            if (dist_nm <= CPR_CONSENSUS_NM) {
                                ac->suspect_agree++;
                                ac->suspect_lat = gLat;  // track latest in cluster
                                ac->suspect_lon = gLon;
                                if (ac->suspect_agree >= CPR_CONSENSUS_COUNT) {
                                    recovered = 1;
                                    via_consensus = 1;
                                }
                            } else {
                                // Doesn't match previous candidate — restart consensus
                                ac->suspect_agree = 1;
                                ac->suspect_lat = gLat;
                                ac->suspect_lon = gLon;
                            }
                        } else {
                            // First candidate in recovery — seed consensus
                            ac->suspect_agree = 1;
                            ac->suspect_lat = gLat;
                            ac->suspect_lon = gLon;
                        }
                    }

                    if (recovered) {
                        ac->lat = gLat;
                        ac->lon = gLon;
                        c->lat = gLat;
                        c->lon = gLon;
                        ac->position_suspect = 0;
                        ac->suspect_count = 0;
                        ac->suspect_agree = 0;
                        ac->good_lat = gLat;
                        ac->good_lon = gLon;
                        ac->good_pos_ts = now;
                        decoded = 1;
                        if (via_consensus)
                            adsb_logq_printf("CPR: %06lX recovered via consensus (%d agreeing globals)\n",
                                                 ac->icao, CPR_CONSENSUS_COUNT);
                        else
                            adsb_logq_printf("CPR: %06lX recovered via global decode\n", ac->icao);
                    } else {
                        ac->suspect_count++;
                        if (ac->suspect_count >= CPR_SUSPECT_GIVE_UP) {
                            ac->has_position = 0;
                            ac->position_suspect = 0;
                            ac->suspect_count = 0;
                            ac->suspect_agree = 0;
                            adsb_logq_printf("CPR: %06lX gave up recovery — resetting position\n",
                                                 ac->icao);
                        }
                    }

                } else if (!decoded) {
                    // Local failed (50nm check) — try global as re-seed
                    if (cpr_jump_check(ac, gLat, gLon, now)) {
                        ac->lat = gLat;
                        ac->lon = gLon;
                        ac->good_lat = gLat;
                        ac->good_lon = gLon;
                        ac->good_pos_ts = now;
                        decoded = 1;
                    } else {
                        // Global re-seed also implausible — enter suspect
                        ac->position_suspect = 1;
                        ac->suspect_count = 1;
                        ac->suspect_agree = 0;
                        ac->suspect_lat = 0;
                        ac->suspect_lon = 0;
                        adsb_logq_printf("CPR: %06lX position suspect — global re-seed jumped\n",
                                             ac->icao);
                    }

                } else if (frame_age <= CPR_TIGHT_AGE_US) {
                    // Keyframe correction: local succeeded, tight global available
                    if (cpr_jump_check(ac, gLat, gLon, now)) {
                        double dlat = ac->lat - gLat;
                        double dlon = (ac->lon - gLon) * cos(ac->lat * M_PI / 180.0);
                        double dist_nm = sqrt(dlat * dlat + dlon * dlon) * 60.0;
                        if (dist_nm > 2.0) {
                            // Local reference has drifted — reseat from global
                            ac->lat = gLat;
                            ac->lon = gLon;
                            ac->good_lat = gLat;
                            ac->good_lon = gLon;
                            ac->good_pos_ts = now;
                        }
                    } else {
                        // Tight global disagrees with good_pos — good_pos may be wrong.
                        // Both local and tight global agree (decoded=1 + tight pair),
                        // but both are far from good_pos. Enter suspect, seed consensus.
                        ac->position_suspect = 1;
                        ac->suspect_count = 1;
                        ac->suspect_agree = 1;
                        ac->suspect_lat = gLat;
                        ac->suspect_lon = gLon;
                        adsb_logq_printf("CPR: %06lX position suspect — tight global disagrees with reference\n",
                                             ac->icao);
                    }
                    // Loose global (2–10s) with local already decoded: skip.
                }
            }
        }
        if (decoded) s_gain_win_pos++;  // track for adaptive gain throughput metric
    }

    // Velocity (DF17, TC 19, subtypes 1-4)
    if (mm->msgtype == 17 && mm->metype == 19) {
        if (mm->mesub == 1 || mm->mesub == 2) {
            ac->speed = mm->velocity;
            // Heading is computed from EW/NS velocity components in mode_s_decode
            // and is always valid when velocity > 0 (heading_is_valid is only
            // meaningful for subtypes 3/4 which carry airborne magnetic heading).
            if (mm->velocity)
                ac->heading = mm->heading;
            // vert_rate raw=0 means "not available" per DO-260B
            if (mm->vert_rate) {
                ac->vert_rate = mm->vert_rate_sign
                              ? -((mm->vert_rate - 1) * 64)
                              :  ((mm->vert_rate - 1) * 64);
            }
        } else if (mm->mesub == 3 || mm->mesub == 4) {
            if (mm->heading_is_valid)
                ac->heading = mm->heading;
        }
    }

    // Squawk (DF5, DF21)
    if (mm->msgtype == 5 || mm->msgtype == 21) {
        ac->squawk = mm->identity;
    }

    // Update message statistics
    s_total_messages++;
    s_msg_count_window++;
    {
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed = now_us - s_msg_window_start;
        if (elapsed >= 1000000LL) {  // 1 second window
            s_msg_rate = (float)s_msg_count_window * 1000000.0f / (float)elapsed;
            s_msg_count_window = 0;
            s_msg_window_start = now_us;
        }
    }

    // The table is up to date. Everything below describes this message to
    // the console, the SD card and MQTT; it works on a copy, so the lock is
    // released here. Up to coh3 it was held to the end of the function:
    // through three double-precision haversines, the text of two lines, and
    // any SD card write the row triggered – while the display and the
    // console waited for the aircraft table.
    const aircraft_t snap = *ac;
    xSemaphoreGive(aircraft_mutex);
    ac = NULL;

    // Aircraft database info on the first message for a new ICAO.
    // Separate log line so we don't change the ADS-B data format.
    // Webapp and firmware scope cache this for detail display.
    // (A lookup that is not in the cache reads the SD card.)
    if (first_msg && aircraft_db_ready()) {
        aircraft_db_entry_t db;
        if (aircraft_db_lookup(icao, &db)) {
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            adsb_logq_printf("%sICAO: %06lX|%s|%s|%s|%s|%s|%s|%c\n",
                _ts, (unsigned long)icao,
                db.reg, db.typecode, db.mfr, db.model, db.owner, db.op_icao,
                db.ac_class ? db.ac_class : '?');
        }
    }

    // Range and bearing from the receiver: once, in single precision, for
    // the console line, the SD row and MQTT.
    const receiver_pos_t rx = adsb_get_receiver_pos();
    const bool have_range = rx.fix_valid && snap.has_position;
    float range_km = 0.0f, bearing = 0.0f;
    if (have_range) adsb_gc(rx.lat, rx.lon, snap.lat, snap.lon, &range_km, &bearing);

    // --- One-line summary with full known state ---
    // The log task writes it with serial_console_print(), so lines are
    // buffered in PSRAM during CMD mode and replayed when returning to LOG mode.
    // Not written at all when the queue has no room for it: the line would
    // be counted as dropped anyway.
    if (!replayed && !adsb_logq_has_room(ADSB_LOG_LINE_MAX)) {
        s_log_dropped_total++;   // the console is not keeping up
    } else if (!replayed) {
    char line[384];
    int pos = 0, avail = sizeof(line);
#define LINE_PUT(expr) do { int n_ = (expr); if (n_ > 0 && n_ < avail) { pos += n_; avail -= n_; } } while (0)

    // Timestamp — shared format with GNSS and Meshy log lines
    LINE_PUT(log_format_timestamp(line + pos, avail));

    // ICAO + callsign
    LINE_PUT(snprintf(line + pos, avail, "%06lX", (unsigned long)icao));
    if (snap.callsign[0]) LINE_PUT(snprintf(line + pos, avail, " %-8s", snap.callsign));

    // Altitude + velocity
    if (snap.altitude)  LINE_PUT(snprintf(line + pos, avail, " %5dft", snap.altitude));
    if (snap.speed)     LINE_PUT(snprintf(line + pos, avail, " %3dkt %03d°", snap.speed, snap.heading));
    if (snap.vert_rate) LINE_PUT(snprintf(line + pos, avail, " %+dfpm", snap.vert_rate));

    // Position + distance from receiver: " (37.8044N,122.2712W) 12.3nm@045°"
    if (snap.has_position) {
        LINE_PUT(snprintf(line + pos, avail, " ("));
        LINE_PUT(adsb_put_fixed(line + pos, avail, fabs(snap.lat), 4, 0, ' '));
        LINE_PUT(snprintf(line + pos, avail, "%c,", snap.lat >= 0 ? 'N' : 'S'));
        LINE_PUT(adsb_put_fixed(line + pos, avail, fabs(snap.lon), 4, 0, ' '));
        LINE_PUT(snprintf(line + pos, avail, "%c)", snap.lon >= 0 ? 'E' : 'W'));
        if (rx.fix_valid) {
            LINE_PUT(snprintf(line + pos, avail, " "));
            LINE_PUT(adsb_put_fixed_f(line + pos, avail, range_km * 0.539957f, 1, 0, ' '));
            LINE_PUT(snprintf(line + pos, avail, "nm@"));
            LINE_PUT(adsb_put_fixed_f(line + pos, avail, bearing, 0, 3, '0'));
            LINE_PUT(snprintf(line + pos, avail, "°"));
        }
    }

    // Squawk
    if (snap.squawk) LINE_PUT(snprintf(line + pos, avail, " SQK:%04d", snap.squawk));

    // Raw hex (compact, at end)
    LINE_PUT(snprintf(line + pos, avail, " ["));
    {
        int msglen = mm->msgbits / 8;
        if (msglen > 14) msglen = 14;
        LINE_PUT(adsb_put_hex(line + pos, avail, mm->msg, msglen));
    }
#undef LINE_PUT
    if (avail > 2) { line[pos++] = ']'; line[pos++] = '\n'; line[pos] = '\0'; }

    // Queued for the log task, never written here: the decoder must not
    // wait for the console.
    adsb_logq_push(line, pos);
    }

    if (!replayed) {
        int64_t t_sd = esp_timer_get_time();
        sd_log_aircraft(&snap, mm, &rx, range_km, bearing);
        s_sdlog_busy_us += (uint32_t)(esp_timer_get_time() - t_sd);
    }

    // Queue aircraft update for MQTT feeder (returns immediately if not connected)
    if (!replayed) {
        mqtt_aircraft_t mqac = {0};
        snprintf(mqac.icao, sizeof(mqac.icao), "%06lX", (unsigned long)icao);
        if (snap.callsign[0])
            strncpy(mqac.callsign, snap.callsign, sizeof(mqac.callsign) - 1);
        mqac.lat = snap.lat;
        mqac.lon = snap.lon;
        mqac.alt_ft = snap.altitude;
        mqac.speed_kt = (int16_t)snap.speed;
        mqac.heading_deg = (int16_t)snap.heading;
        mqac.vert_rate_fpm = (int16_t)snap.vert_rate;
        mqac.has_pos = snap.has_position ? true : false;
        mqac.ts_us = esp_timer_get_time();
        if (have_range) {
            mqac.dist_nm = range_km * 0.539957f;
            mqac.bearing_deg = (int16_t)bearing;
        }
        mqtt_feeder_update_aircraft(&mqac);
    }
}

// ============================================================
// demodulate — uses pre-allocated magnitude buffer
// ============================================================

// Copy the options asked for into the decoder state. Called by the reader
// task after mode_s_init() (no decoder task exists then) and by the decoder
// task between two runs - never while the decoder is inside a run.
static void decoder_apply_options(void)
{
    const uint32_t req = s_dec_req;
    adsb_decoder_opts_t o;
    dec_unpack(req, &o);
    state.fix2 = o.fix2;
    state.strict_es = o.strict;
    state.fix2_known_icao = o.known_addr;
    state.soft_fix = o.soft; // soft rescue; used by the 2.4 Msps demodulator only
    bool coh = o.coh && ADSB_DEMOD_IS_2400;
    if (coh && s_coh_work == NULL) {
        s_coh_work = heap_caps_malloc(mode_s_coh_work_size(), MALLOC_CAP_SPIRAM);
        if (s_coh_work == NULL)
            ESP_LOGE(TAG, "coherent look: no memory for its %u bytes - it stays off", (unsigned)mode_s_coh_work_size());
    }
    s_coh_active = mode_s_coh_enable(&state, s_coh_work, coh) != 0;
    s_dec_applied = req;
    dec_counts_now(&s_dec_line_cnt);   // the next DECODER: line counts from here
    s_dec_line_us = esp_timer_get_time();
}

void adsb_decoder_set(const adsb_decoder_opts_t *opts)
{
    uint32_t v = DEC_SOFT(opts->soft > 8 ? 8 : opts->soft);
    if (opts->fix2) v |= DEC_FIX2;
    if (opts->strict) v |= DEC_STRICT;
    if (opts->known_addr) v |= DEC_KNOWN;
    if (opts->coh) v |= DEC_COH;
    s_dec_req = v;
    if (s_decode_task_hdl) xTaskNotifyGive(s_decode_task_hdl);   // do not wait for the next batch of samples
}

void adsb_decoder_get(adsb_decoder_opts_t *opts, adsb_decoder_info_t *info)
{
    if (opts) dec_unpack(s_dec_req, opts);
    if (info) {
        const bool running = (s_decode_task_hdl != NULL) && !s_decode_stop;
        memset(info, 0, sizeof(*info));
        info->applied = running && (s_dec_applied == s_dec_req);
        info->coh_active = running && s_coh_active;
        info->fix1 = state.stat_fix1;
        info->fix2 = state.stat_fix2;
        info->soft = state.stat_soft_fix;
        info->addr_refused = state.stat_fix2_icao_rej;
        info->strict_refused = state.stat_strict_rej;
        info->coh_try = state.stat_coh_try;
        info->coh_abort = state.stat_coh_abort;
        info->coh_ok = state.stat_coh_ok;
        info->coh_soft = state.stat_coh_soft;
        info->stack_free = running ? s_decode_stack_free : 0;
    }
}

void adsb_decoder_describe(const adsb_decoder_opts_t *o, char *buf, size_t len)
{
    char gates[40] = "";
    if (o->fix2 || o->soft)
        snprintf(gates, sizeof(gates), " (%s%s)", o->known_addr ? "known address" : "any address",
                 o->strict ? ", strict" : "");
    else if (o->strict)
        snprintf(gates, sizeof(gates), ", strict");
    snprintf(buf, len, "1-bit fix%s%s%s%s", o->soft ? ", soft rescue" : "", o->fix2 ? ", 2-bit fix" : "",
             gates, o->coh ? ", coherent look" : "");
}

// Mirror the decoder's DF18 counters for adsb_get_stats() and, once a
// minute, log them if any DF18 arrived during that minute. The line starts
// with "DF18:" directly after the timestamp, so the web app's aircraft-line
// matcher (six hex digits there) cannot take it for an aircraft.
static void df18_tick(void)
{
    uint32_t total = state.stat_df18;
    s_df18_total   = total;
    s_df18_tisb    = state.stat_df18_cf[2] + state.stat_df18_cf[3] + state.stat_df18_cf[5];
    s_df18_adsr    = state.stat_df18_cf[6];
    s_df18_adsb_nt = state.stat_df18_cf[0] + state.stat_df18_cf[1];
    s_df18_other   = state.stat_df18_cf[4] + state.stat_df18_cf[7];

    int64_t now = esp_timer_get_time();
    if (s_df18_win_start == 0 || total < s_df18_win_base) {
        // first call, or the decoder counters were reset (receiver restart)
        s_df18_win_start = now;
        s_df18_win_base = total;
        return;
    }
    if (now - s_df18_win_start >= 60000000LL) {
        s_df18_last_min = total - s_df18_win_base;
        s_df18_win_base = total;
        s_df18_win_start = now;
        if (s_df18_last_min > 0) {
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            adsb_logq_printf("%sDF18: %lu in last 60 s, %lu total (TIS-B %lu, ADS-R %lu, ADS-B-NT %lu, other %lu)\n",
                                 _ts, (unsigned long)s_df18_last_min, (unsigned long)total,
                                 (unsigned long)s_df18_tisb, (unsigned long)s_df18_adsr,
                                 (unsigned long)s_df18_adsb_nt, (unsigned long)s_df18_other);
        }
    }
}

void demodulate(uint8_t *source, int length)
{
    if (!s_mag_buf) return;
    // Streaming detection: carries a short magnitude tail between chunks so
    // frames straddling a chunk boundary are decoded (they used to be lost),
    // and maintains an absolute sample counter per frame
    // (mode_s_frame_sample_index()). Host-tested to be identical to
    // processing the whole stream as one buffer (harness/check_stream.py).
    // s_mag_buf holds DEFAULT_BUF_LENGTH/2 + MODE_S_STREAM_TAIL_MAX samples.
    int64_t t0 = esp_timer_get_time();
    if (ADSB_DEMOD_IS_2400)
        mode_s_detect_stream_2400(&state, source, (uint32_t)length, s_mag_buf, on_msg);
    else
        mode_s_detect_stream(&state, source, (uint32_t)length, s_mag_buf, on_msg);
    int64_t t1 = esp_timer_get_time();
    df18_tick();
    if (s_replay_counting) s_replay_busy_us += (uint64_t)(t1 - t0);
    s_capture_bytes_win += (uint32_t)length;

    // Decode load over ~5 s windows (see adsb_stats_t.decode_load)
    s_decode_busy_us += (uint32_t)(t1 - t0);
    if (s_window_restart) {
        s_window_restart = false;
        s_decode_win_start = 0;
    }
    if (s_decode_win_start == 0) {
        s_decode_win_start = t1;
        s_decode_busy_us = 0;
        s_sdlog_busy_us = 0;
        s_capture_bytes_win = 0;
        s_win_log_lines0 = s_log_lines_total;
        s_win_log_bytes0 = s_log_bytes_total;
        s_win_log_dropped0 = s_log_dropped_total;
    } else if (t1 - s_decode_win_start >= 5000000LL) {
        const uint64_t win_us = (uint64_t)(t1 - s_decode_win_start);
        const uint32_t dec_pm = (uint32_t)(((uint64_t)s_decode_busy_us * 1000ULL) / win_us);
        const uint32_t sd_pm  = (uint32_t)(((uint64_t)s_sdlog_busy_us * 1000ULL) / win_us);
        // Two bytes per sample. What the decoder was given against what the
        // dongle produced: short of 100 % by whatever the USB side or a
        // full ring lost. (It can read a little over 100 % in a window in
        // which the decoder caught up on a backlog.)
        const uint64_t produced = win_us * (uint64_t)ADSB_SAMPLE_RATE * 2ULL / 1000000ULL;
        const uint32_t cap_pm = produced ? (uint32_t)(((uint64_t)s_capture_bytes_win * 1000ULL) / produced) : 0;
        const uint32_t lines_total = s_log_lines_total, bytes_total = s_log_bytes_total, dropped_total = s_log_dropped_total;
        const uint32_t lines_s = (uint32_t)(((uint64_t)(lines_total - s_win_log_lines0) * 1000000ULL) / win_us);
        const uint32_t bytes_s = (uint32_t)(((uint64_t)(bytes_total - s_win_log_bytes0) * 1000000ULL) / win_us);
        s_capture_permille     = cap_pm;
        s_decode_load_permille = dec_pm;
        s_sdlog_load_permille  = sd_pm;
        s_log_lines_s = lines_s;
        s_log_bytes_s = bytes_s;
        s_log_dropped_win = dropped_total - s_win_log_dropped0;
        if (cap_pm < s_min_capture_permille) s_min_capture_permille = cap_pm;
        if (dec_pm > s_peak_decode_permille) s_peak_decode_permille = dec_pm;
        s_decode_stack_free = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        // Once a minute, on the log itself, so a recorded log shows how
        // the receiver was doing at any point in a session.
        if (++s_load_line_windows >= 12) {
            const uint32_t usb_pm = s_usb_capture_permille;
            s_load_line_windows = 0;
            char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
            adsb_logq_printf("%sLOAD: capture %lu.%lu%% (USB %lu.%lu%%), decode %lu.%lu%% of one core (SD log %lu.%lu%%), "
                             "ring max %lu%% dropped %lu, USB starved %lu, "
                             "log %lu lines/s %lu B/s dropped %lu, %.0f msg/s\n",
                             _ts, (unsigned long)(cap_pm / 10), (unsigned long)(cap_pm % 10),
                             (unsigned long)(usb_pm / 10), (unsigned long)(usb_pm % 10),
                             (unsigned long)(dec_pm / 10), (unsigned long)(dec_pm % 10),
                             (unsigned long)(sd_pm / 10), (unsigned long)(sd_pm % 10),
                             (unsigned long)(s_ring.max_fill * 100 / (ADSB_RING_SLOTS - 1)),
                             (unsigned long)s_ring.dropped, (unsigned long)s_usb_starved,
                             (unsigned long)lines_s, (unsigned long)bytes_s, (unsigned long)dropped_total,
                             (double)s_msg_rate);
            // And what the decoder's repair stages did since the last such
            // line (or since the options changed). "DECODER:" cannot be
            // taken for an aircraft line: 'O' is not a hex digit.
            {
                adsb_decoder_opts_t o;
                dec_counts_t c;
                char what[96];
                dec_unpack(s_dec_applied, &o);
                o.coh = s_coh_active;
                adsb_decoder_describe(&o, what, sizeof(what));
                dec_counts_now(&c);
                adsb_logq_printf("%sDECODER: %s; in the last %lu s: repaired 1-bit %lu, soft %lu, 2-bit %lu; "
                                 "coherent look %lu re-slices, %lu stopped early, %lu frames recovered\n",
                                 _ts, what, (unsigned long)((t1 - s_dec_line_us) / 1000000LL),
                                 (unsigned long)(c.fix1 - s_dec_line_cnt.fix1), (unsigned long)(c.soft - s_dec_line_cnt.soft),
                                 (unsigned long)(c.fix2 - s_dec_line_cnt.fix2),
                                 (unsigned long)(c.coh_try - s_dec_line_cnt.coh_try),
                                 (unsigned long)(c.coh_abort - s_dec_line_cnt.coh_abort),
                                 (unsigned long)(c.coh_ok - s_dec_line_cnt.coh_ok));
                s_dec_line_cnt = c;
                s_dec_line_us = t1;
            }
        }
        s_decode_busy_us = 0;
        s_sdlog_busy_us = 0;
        s_capture_bytes_win = 0;
        s_win_log_lines0 = lines_total;
        s_win_log_bytes0 = bytes_total;
        s_win_log_dropped0 = dropped_total;
        s_decode_win_start = t1;
    }
}

// ============================================================
// Log line queue — see the declarations near the top of the file
// ============================================================

// Records are a 2-byte length followed by the text. Head and tail count
// bytes without wrapping; the position in the buffer is the count masked.
static void logq_write(uint32_t at, const char *src, uint32_t n)
{
    at &= (ADSB_LOGQ_BYTES - 1);
    uint32_t first = ADSB_LOGQ_BYTES - at;
    if (first > n) first = n;
    memcpy(s_logq + at, src, first);
    memcpy(s_logq, src + first, n - first);
}

static void logq_read(uint32_t at, char *dst, uint32_t n)
{
    at &= (ADSB_LOGQ_BYTES - 1);
    uint32_t first = ADSB_LOGQ_BYTES - at;
    if (first > n) first = n;
    memcpy(dst, s_logq + at, first);
    memcpy(dst + first, s_logq, n - first);
}

// Queue one line for the console. Never blocks. Called from the decoder
// task only (one producer).
static void adsb_logq_push(const char *line, int len)
{
    if (len <= 0) return;
    if (s_replay_counting) return;   // a replay's messages stay off the log
    if (len > ADSB_LOG_LINE_MAX - 1) len = ADSB_LOG_LINE_MAX - 1;
    if (s_logq == NULL || s_log_task_hdl == NULL) {
        // No queue (allocation failed at start-up): write directly, as
        // every release before r9 did.
        if (serial_console_print("%.*s", len, line)) {
            s_log_lines_total++;
            s_log_bytes_total += (uint32_t)len + 1;
        }
        return;
    }
    uint32_t head = atomic_load_explicit(&s_logq_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s_logq_tail, memory_order_acquire);
    if (ADSB_LOGQ_BYTES - (head - tail) < (uint32_t)len + 2) {
        s_log_dropped_total++;   // the console is not keeping up
        return;
    }
    char hdr[2] = { (char)(len & 0xff), (char)(len >> 8) };
    logq_write(head, hdr, 2);
    logq_write(head + 2, line, (uint32_t)len);
    atomic_store_explicit(&s_logq_head, head + 2 + (uint32_t)len, memory_order_release);
    xTaskNotifyGive(s_log_task_hdl);
}

// Room in the queue for a line of `len` characters? (Asked before the text
// of a line is built: at 300 messages a second on a 115200 baud console two
// lines in three were built and then dropped.) True when there is no queue,
// where lines are written directly.
static bool adsb_logq_has_room(uint32_t len)
{
    if (s_logq == NULL || s_log_task_hdl == NULL) return true;
    uint32_t head = atomic_load_explicit(&s_logq_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s_logq_tail, memory_order_acquire);
    return ADSB_LOGQ_BYTES - (head - tail) >= len + 2;
}

static void adsb_logq_printf(const char *fmt, ...)
{
    char tmp[ADSB_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    adsb_logq_push(tmp, n);
}

// Writes queued lines to the console. This is the task that waits for the
// UART now; it runs below the decoder and the application tasks.
// The console UART is polled (no driver): a write that does not fit its
// transmit FIFO spins until it does. A task that spins at this priority
// would take a whole core from the display whenever the log is busy, so
// the writer keeps an estimate of when the FIFO will have room for the
// next line and sleeps until then. It still fills the UART completely.
// The console's speed is a property of the session (serial_console_baud():
// 115200 unless a host has asked for more), so the estimate is made per
// line. It sleeps in whole ticks (1 ms): at 921600 baud, where the FIFO
// empties in 1.4 ms, that is 450 to 500 lines a second.
#ifdef CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define ADSB_LOG_UART         1
#define ADSB_LOG_UART_FIFO    127
// microseconds that n characters take on the wire (10 bits each)
static inline int64_t adsb_log_wire_us(uint32_t n, uint32_t baud) { return ((int64_t)n * 10000000LL) / (int64_t)baud; }
#endif

static void adsb_log_task(void *arg)
{
    static char line[ADSB_LOG_LINE_MAX];
#ifdef ADSB_LOG_UART
    int64_t drain_at = 0;   // when the bytes written so far will have left the FIFO
    uint32_t at_baud = 0;   // the speed that estimate was made at
#endif
    for (;;) {
        // The console has just changed speed and the host has not confirmed
        // it yet: what is written now would be lost. Lines wait in the queue.
        if (serial_console_link_quiet()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t tail = atomic_load_explicit(&s_logq_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&s_logq_head, memory_order_acquire);
        if (head == tail) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        unsigned char hdr[2];
        logq_read(tail, (char *)hdr, 2);
        uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8);
        if (len >= ADSB_LOG_LINE_MAX) len = ADSB_LOG_LINE_MAX - 1;  // cannot happen; never overrun the buffer
        logq_read(tail + 2, line, len);
        line[len] = '\0';
        atomic_store_explicit(&s_logq_tail, tail + 2 + len, memory_order_release);
#ifdef ADSB_LOG_UART
        const uint32_t out = len + 1;   // + the CR the console adds
        const uint32_t baud = serial_console_baud();
        if (baud != at_baud) { at_baud = baud; drain_at = 0; }   // the FIFO was emptied for the change
        if (serial_console_log_enabled()) {
            int64_t now = esp_timer_get_time();
            if (drain_at < now) drain_at = now;
            // Room for the whole line, or an empty FIFO for a longer one
            const uint32_t need = out < ADSB_LOG_UART_FIFO ? out : ADSB_LOG_UART_FIFO;
            int64_t room_at = drain_at - adsb_log_wire_us(ADSB_LOG_UART_FIFO - need, baud);
            if (room_at > now) vTaskDelay(pdMS_TO_TICKS((uint32_t)((room_at - now + 999) / 1000)));
        }
#endif
        // In command mode this buffers the line for replay instead of
        // printing it, as before.
        if (serial_console_print("%s", line)) {
            s_log_lines_total++;
            s_log_bytes_total += len + 1;   // + the CR the console adds
#ifdef ADSB_LOG_UART
            int64_t now = esp_timer_get_time();
            if (drain_at < now) drain_at = now;
            drain_at += adsb_log_wire_us(out, baud);
            // On return at most a FIFO's worth is still waiting
            if (drain_at > now + adsb_log_wire_us(ADSB_LOG_UART_FIFO, baud))
                drain_at = now + adsb_log_wire_us(ADSB_LOG_UART_FIFO, baud);
#endif
        }
    }
}

// Create the queue and its task on first use. On failure lines are
// written directly (adsb_logq_push).
static void adsb_log_start(void)
{
    if (s_log_task_hdl != NULL) return;
    if (s_logq == NULL) {
        s_logq = heap_caps_malloc(ADSB_LOGQ_BYTES, MALLOC_CAP_SPIRAM);
        atomic_init(&s_logq_head, 0);
        atomic_init(&s_logq_tail, 0);
    }
    if (s_logq == NULL) {
        ESP_LOGW(TAG, "log queue alloc failed — log lines are written from the decoder");
        return;
    }
    TaskHandle_t h = NULL;
    if (xTaskCreateWithCaps(adsb_log_task, "adsb_log", ADSB_LOG_TASK_STACK, NULL,
                            ADSB_LOG_TASK_PRIO, &h, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "log task create failed — log lines are written from the decoder");
        return;
    }
    s_log_task_hdl = h;
}

// ============================================================
// Observation mark and replay
// ============================================================

void adsb_stats_mark(void)
{
    s_mark_ring_dropped = s_ring_mem ? s_ring.dropped : 0;
    s_mark_usb_starved  = s_usb_starved;
    s_mark_log_dropped  = s_log_dropped_total;
    s_min_capture_permille = 1000;
    s_peak_decode_permille = 0;
    s_window_restart = true;   // the window in progress straddles the mark: discard it
    s_mark_us = esp_timer_get_time();
}

// Forget every tracked aircraft. A replay starts and ends with this, so
// live and recorded traffic never share the table.
static void aircraft_table_clear(void)
{
    if (!aircraft_mutex) return;
    xSemaphoreTake(aircraft_mutex, portMAX_DELAY);
    memset(aircraft_table, 0, sizeof(aircraft_table));
    memset(cpr_cache, 0, sizeof(cpr_cache));
    xSemaphoreGive(aircraft_mutex);
}

// Called by the decoder task whenever it finds the ring empty. Everything
// a replay changes in the decoder's state is changed here, by the decoder
// itself, at a moment when it has no samples in hand.
static void replay_service_idle(void)
{
    if (s_replay_phase == 1) {
        aircraft_table_clear();
        mode_s_cache_clock(&state, ADSB_SAMPLE_RATE);  // the recording's clock, not the wall's
        decoder_apply_options();   // and the coherent look's noise reference starts afresh, as on a PC
        s_replay_msgs = 0;
        s_replay_busy_us = 0;
        dec_counts_now(&s_replay_cnt0);
        s_replay_counting = true;
        s_replay_phase = 2;
    } else if (s_replay_phase == 3) {
        dec_counts_now(&s_replay_cnt1);
        s_replay_counting = false;
        mode_s_cache_clock(&state, 0);
        decoder_apply_options();   // nothing of the recording's levels is carried into live reception
        aircraft_table_clear();
        s_replay_phase = 0;
    }
}

static void replay_fail(const char *msg)
{
    snprintf(s_replay.error, sizeof(s_replay.error), "%s", msg);
    s_replay.ok = false;
}

static void adsb_replay_task(void *arg)
{
    FILE *f = NULL;
    uint8_t *buf = heap_caps_malloc(ADSB_USB_XFER_BYTES, MALLOC_CAP_SPIRAM);
    const int64_t t_start = esp_timer_get_time();
    const uint32_t dropped0 = s_ring.dropped;
    bool taken_over = false;

    s_replay.ok = true;
    if (!buf) { replay_fail("out of memory"); goto out; }
    f = fopen(s_replay.path, "rb");
    if (!f) { replay_fail("cannot open the file"); goto out; }
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0) s_replay.file_bytes = (uint64_t)sz;
    }
    rewind(f);

    // Take the ring's input over from the live stream: the USB callback
    // stops storing (it finishes within a millisecond), then the decoder
    // empties the ring and switches itself over.
    s_replay_feed = true;
    taken_over = true;
    vTaskDelay(pdMS_TO_TICKS(50));
    s_replay_phase = 1;
    for (int i = 0; s_replay_phase != 2; i++) {
        if (s_decode_task_hdl == NULL || s_decode_stop || i > 500) {
            replay_fail("the decoder did not take over (is the receiver running?)");
            goto out;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    sample_ring_note_loss(&s_ring, 0);   // the recording does not follow the live samples

    for (;;) {
        if (s_replay_stop) { replay_fail("stopped"); break; }
        if (s_decode_task_hdl == NULL || s_decode_stop) { replay_fail("the receiver stopped"); break; }
        size_t n = fread(buf, 1, ADSB_USB_XFER_BYTES, f);
        n &= ~(size_t)1;   // whole I/Q pairs
        if (n == 0) break;
        // Never overflow: wait for the decoder. This is what makes the
        // result repeatable.
        while (sample_ring_fill(&s_ring) >= ADSB_RING_SLOTS - 2 && !s_replay_stop &&
               s_decode_task_hdl != NULL && !s_decode_stop)
            vTaskDelay(1);
        sample_ring_put(&s_ring, buf, (uint32_t)n, esp_timer_get_time());
        s_replay.fed_bytes += n;
        if (s_decode_task_hdl != NULL && sample_ring_fill(&s_ring) >= ADSB_DECODE_BATCH_SLOTS)
            xTaskNotifyGive(s_decode_task_hdl);
    }

out:
    if (taken_over) {
        // Let the decoder finish what is in the ring and switch back.
        if (s_replay_phase == 2) {
            s_replay_phase = 3;
            for (int i = 0; s_replay_phase != 0 && i < 3000; i++) {
                if (s_decode_task_hdl == NULL || s_decode_stop) break;
                xTaskNotifyGive(s_decode_task_hdl);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (s_replay_phase != 0) {
            // The decoder task has gone (dongle removed): undo by hand.
            dec_counts_now(&s_replay_cnt1);
            s_replay_counting = false;
            s_replay_phase = 0;
        }
        {
            adsb_decoder_opts_t o;
            dec_unpack(s_dec_applied, &o);
            o.coh = s_coh_active;
            adsb_decoder_describe(&o, s_replay.decoder, sizeof(s_replay.decoder));
        }
        s_replay.fix1      = s_replay_cnt1.fix1 - s_replay_cnt0.fix1;
        s_replay.fix2      = s_replay_cnt1.fix2 - s_replay_cnt0.fix2;
        s_replay.soft      = s_replay_cnt1.soft - s_replay_cnt0.soft;
        s_replay.coh_try   = s_replay_cnt1.coh_try - s_replay_cnt0.coh_try;
        s_replay.coh_abort = s_replay_cnt1.coh_abort - s_replay_cnt0.coh_abort;
        s_replay.coh_ok    = s_replay_cnt1.coh_ok - s_replay_cnt0.coh_ok;
        s_replay.messages       = s_replay_msgs;
        s_replay.decode_busy_ms = (uint32_t)(s_replay_busy_us / 1000);
        s_replay.ring_dropped   = s_ring.dropped - dropped0;
        s_replay.signal_ms      = (uint32_t)(s_replay.fed_bytes / 2 * 1000ULL / ADSB_SAMPLE_RATE);
        s_live_gap_pending = true;
        s_replay_feed = false;
        adsb_stats_mark();   // the extremes of the last minutes describe the replay, not the receiver
    }
    s_replay.wall_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
    if (f) fclose(f);
    heap_caps_free(buf);

    {
        char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
        if (s_replay.ok) {
            uint32_t share_pm = s_replay.signal_ms ? (uint32_t)((uint64_t)s_replay.decode_busy_ms * 1000ULL / s_replay.signal_ms) : 0;
            serial_console_print("%sREPLAY: %s: %llu bytes, %lu.%lu s of signal, %lu messages, "
                                 "decoder busy %lu ms = %lu.%lu%% of one core at real time, "
                                 "%lu transfers dropped, took %lu s\n",
                                 _ts, s_replay.path, (unsigned long long)s_replay.fed_bytes,
                                 (unsigned long)(s_replay.signal_ms / 1000), (unsigned long)(s_replay.signal_ms % 1000 / 100),
                                 (unsigned long)s_replay.messages, (unsigned long)s_replay.decode_busy_ms,
                                 (unsigned long)(share_pm / 10), (unsigned long)(share_pm % 10),
                                 (unsigned long)s_replay.ring_dropped, (unsigned long)(s_replay.wall_ms / 1000));
            // A second line, so that the first keeps the form it had in r10.
            serial_console_print("%sREPLAY: decoder: %s; repaired 1-bit %lu, soft %lu, 2-bit %lu; "
                                 "coherent look %lu re-slices, %lu stopped early, %lu frames recovered\n",
                                 _ts, s_replay.decoder, (unsigned long)s_replay.fix1, (unsigned long)s_replay.soft,
                                 (unsigned long)s_replay.fix2, (unsigned long)s_replay.coh_try,
                                 (unsigned long)s_replay.coh_abort, (unsigned long)s_replay.coh_ok);
        } else {
            serial_console_print("%sREPLAY: %s: failed: %s (%llu bytes fed, %lu messages)\n",
                                 _ts, s_replay.path, s_replay.error,
                                 (unsigned long long)s_replay.fed_bytes, (unsigned long)s_replay.messages);
        }
    }
    s_replay.done = true;
    s_replay.running = false;
    vTaskDeleteWithCaps(NULL);   // a task with a PSRAM stack ends itself this way
}

bool adsb_replay_start(const char *path, char *err, size_t err_len)
{
    const char *why = NULL;
    if (!path || !path[0])                        why = "no file given";
    else if (s_replay.running)                    why = "a replay is already running";
    else if (s_decode_task_hdl == NULL || !s_ring_mem || !s_rtlsdr_connected)
                                                  why = "the receiver is not running (attach the dongle first)";
    if (!why) {
        memset(&s_replay, 0, sizeof(s_replay));
        snprintf(s_replay.path, sizeof(s_replay.path), "%s", path);
        s_replay.running = true;
        s_replay_stop = false;
        if (xTaskCreateWithCaps(adsb_replay_task, "adsb_replay", ADSB_REPLAY_TASK_STACK, NULL,
                                ADSB_REPLAY_TASK_PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_replay.running = false;
            why = "could not start the replay task";
        }
    }
    if (why) {
        if (err && err_len) snprintf(err, err_len, "%s", why);
        return false;
    }
    return true;
}

void adsb_replay_stop(void)
{
    if (s_replay.running) s_replay_stop = true;
}

void adsb_replay_get(adsb_replay_status_t *out)
{
    if (!out) return;
    *out = s_replay;
    if (s_replay.running) {
        // live figures while it runs
        out->messages = s_replay_msgs;
        out->wall_ms = 0;
    }
}

// ============================================================
// USB transfer -> sample ring (reader task context)
// ============================================================

// One finished USB transfer. Runs inside rtlsdr_stream_poll(), in the
// reader task. Everything here must be quick: while it runs, the
// transfer it was called for is not queued.
static void usb_stream_data_cb(const uint8_t *data, int len, int64_t t_us, bool starved, void *ctx)
{
    if (s_replay_feed) {
        // A recording is being replayed: the replay task is the ring's
        // only writer. Keep reading the dongle, discard what it sends.
        s_usb_prev_cb_us = t_us;
        s_usb_bytes_total += (uint32_t)len;
        return;
    }
    if (s_live_gap_pending) {
        s_live_gap_pending = false;
        sample_ring_note_loss(&s_ring, 0);   // live samples do not follow the recording
    }
    if (starved) {
        // No other transfer was still queued: the dongle was not being
        // read from the moment the last of them finished until now. The
        // previous callback ran at s_usb_prev_cb_us with every transfer
        // queued again, so they kept the endpoint busy for one transfer
        // period each; the rest of the elapsed time is the loss. This is
        // an estimate – it is off by however late that previous callback
        // was – and the capture figure in adsb status is the measurement.
        if (s_usb_prev_cb_us != 0) {
            const int64_t period_us = (int64_t)(ADSB_USB_XFER_BYTES / 2) * 1000000LL / ADSB_SAMPLE_RATE;
            int64_t idle_us = (t_us - s_usb_prev_cb_us) - (int64_t)s_usb_xfers * period_us;
            uint32_t lost = idle_us > 0 ? (uint32_t)(idle_us * (int64_t)ADSB_SAMPLE_RATE / 1000000LL) : 0;
            // With a single transfer (memory was short) every completion
            // finds the endpoint dry; count only those that cost samples.
            if (s_usb_xfers > 1 || lost > 0) {
                sample_ring_note_loss(&s_ring, lost);
                s_usb_starved++;
            }
        }
    }
    s_usb_prev_cb_us = t_us;
    s_usb_bytes_total += (uint32_t)len;
    sample_ring_put(&s_ring, data, (uint32_t)len, t_us);   // a full ring drops it and counts
    if (s_decode_task_hdl != NULL && sample_ring_fill(&s_ring) >= ADSB_DECODE_BATCH_SLOTS)
        xTaskNotifyGive(s_decode_task_hdl);
}

// ============================================================
// Decoder task — takes samples from the ring and decodes them
// ============================================================

static void adsb_decode_task(void *arg)
{
    bool waited = false;
    int64_t last_rest_us = esp_timer_get_time();
    ESP_LOGI(TAG, "ADSB decoder task started");
    while (!s_decode_stop) {
        // Let a few transfers collect, so the decoder works in long runs:
        // one bounded wait, then take whatever is there.
        if (!waited && sample_ring_fill(&s_ring) < ADSB_DECODE_BATCH_SLOTS) {
            waited = true;
            int64_t t_wait = esp_timer_get_time();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            int64_t t_woke = esp_timer_get_time();
            if (t_woke - t_wait >= 1000) last_rest_us = t_woke;   // it really slept
            continue;
        }
        waited = false;

        // A decoder that cannot keep up never waits above, and would then
        // hold a core for good: the tasks below it on that core, and the
        // idle task the watchdog watches, would not run. One tick of rest
        // every 200 ms costs half a percent and prevents that.
        if (esp_timer_get_time() - last_rest_us > 200000) {
            vTaskDelay(1);
            last_rest_us = esp_timer_get_time();
        }

        // Options changed from the console are taken over here, between
        // two runs of the decoder.
        if (s_dec_applied != s_dec_req) decoder_apply_options();

        const uint8_t *data;
        uint32_t bytes, lost;
        bool gap;
        int64_t t_us;
        uint32_t n = sample_ring_peek(&s_ring, DEFAULT_BUF_LENGTH, &data, &bytes, &gap, &lost, &t_us);
        if (n == 0) {
            replay_service_idle();   // the ring is empty: a replay may start or end here
            continue;
        }
        (void)t_us;   // the time stamp is for the timebase work (ROADMAP-AVIATION.md 4.1)

        if (gap) {
            // Samples were lost before this run: do not join it to the
            // previous one, and keep the sample count honest.
            mode_s_stream_gap(&state, lost);
            s_decode_gaps++;
        }
        // Decoded in place; the slots are given back afterwards.
        demodulate((uint8_t *)data, (int)bytes);
        sample_ring_release(&s_ring, n);

        // One-shot health check after first successful read
        {
            static bool first_read_logged = false;
            if (!first_read_logged) {
                first_read_logged = true;
                ESP_LOGI(TAG, "First read OK, stack HWM: %u free, heap: %s",
                         (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                         heap_caps_check_integrity_all(true) ? "OK" : "CORRUPT");
            }
        }

        // Adaptive gain evaluation — phase-dependent timing. Paused while a
        // recording is replayed: its statistics say nothing about the dongle.
        if (s_replay_feed) {
            s_gain_last_eval = esp_timer_get_time();
            s_gl_settle_until = s_gain_last_eval + GAIN_SETTLE_US;   // and what it counted is thrown away after it
        } else if (ADSB_DEMOD_IS_2400) {
            // Levels and yield (gain_ctl.c): adaptive, fixed-gain report and sweep
            gain_level_tick(&state);
        } else if (s_gain_auto) {
            // The error-rate loop, for the 2.0 Msps demodulator, whose
            // preamble test lets few enough candidates through that
            // "candidates" and "messages" are close to the same thing.
            int64_t now_gain = esp_timer_get_time();
            int64_t elapsed = now_gain - s_gain_last_eval;
            uint32_t msgs = state.stat_crc_ok + state.stat_crc_fail;
            bool should_eval = false;

            // Warmup: let decoder stabilize before first evaluation
            if (s_gain_warmup && elapsed < GAIN_WARMUP_US) {
                // Still warming up — skip
            } else {
                if (s_gain_warmup) {
                    // Warmup just ended — flush accumulated counters so first real window starts clean
                    s_gain_warmup = false;
                    state.stat_crc_ok = 0;
                    state.stat_crc_fail = 0;
                    state.stat_preambles = 0;
                    state.stat_signal_sum = 0;
                    state.stat_delta_sum = 0;
                    s_gain_win_pos = 0;
                    s_gain_last_eval = now_gain;
                    ESP_LOGI("GAIN", "Warmup complete — starting adaptive gain");
                } else if (s_gain_phase == 1) {
                    // Phase 1: fast — every 10s or 200 messages
                    if (elapsed >= GAIN_P1_PERIOD_US || msgs >= GAIN_P1_MSG_COUNT)
                        should_eval = true;
                } else {
                    // Phase 2: steady — every 60s
                    if (elapsed >= GAIN_P2_PERIOD_US)
                        should_eval = true;
                }

                if (should_eval) {
                    gain_evaluate(&state);
                    s_gain_last_eval = now_gain;
                }
            }
        }

        // Manual-mode heartbeat: log stats every 60s for webapp visibility
        // (2.0 Msps demodulator; gain_level_tick() writes it otherwise)
        if (!s_gain_auto && !ADSB_DEMOD_IS_2400) {
            static int64_t s_manual_hb_last = 0;
            static uint32_t s_manual_hb_ok = 0;
            static uint32_t s_manual_hb_fail = 0;
            static uint32_t s_manual_hb_pos = 0;
            static uint64_t s_manual_hb_signal = 0;
            static uint64_t s_manual_hb_delta = 0;
            int64_t now_hb = esp_timer_get_time();
            if (s_manual_hb_last == 0) s_manual_hb_last = now_hb;  // first call
            if (now_hb - s_manual_hb_last >= GAIN_P2_PERIOD_US) {
                float elapsed_s = (now_hb - s_manual_hb_last) / 1e6f;
                uint32_t d_ok = state.stat_crc_ok - s_manual_hb_ok;
                uint32_t d_fail = state.stat_crc_fail - s_manual_hb_fail;
                uint32_t d_pos = s_gain_win_pos - s_manual_hb_pos;
                uint64_t d_signal = state.stat_signal_sum - s_manual_hb_signal;
                uint64_t d_delta_sum = state.stat_delta_sum - s_manual_hb_delta;
                float err = (d_ok + d_fail > 0) ? (float)d_fail / (d_ok + d_fail) : 0;
                float pos_rate = (elapsed_s > 0) ? d_pos / elapsed_s : 0;
                uint32_t hb_avg_sig = d_ok > 0 ? (uint32_t)(d_signal / d_ok) : 0;
                uint32_t hb_avg_del = d_ok > 0 ? (uint32_t)(d_delta_sum / d_ok) : 0;
                s_manual_hb_last = now_hb;
                s_manual_hb_ok = state.stat_crc_ok;
                s_manual_hb_fail = state.stat_crc_fail;
                s_manual_hb_pos = s_gain_win_pos;
                s_manual_hb_signal = state.stat_signal_sum;
                s_manual_hb_delta = state.stat_delta_sum;
                char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
                serial_console_print("%sGAIN: [Manual] %.1f dB, err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu\n",
                                     _ts, R820T_GAINS[s_gain_idx] / 10.0,
                                     err * 100, pos_rate, s_max_range_nm,
                                     (unsigned long)hb_avg_sig, (unsigned long)hb_avg_del);
            }
        }
    }

    // Tell the reader task we are out of the decoder and hold nothing,
    // then wait to be deleted.
    xSemaphoreGive(s_decode_done);
    vTaskSuspend(NULL);
}

// ============================================================
// ADSB reader task — opens the RTL-SDR, then keeps its sample stream
// moving into the ring until the device goes away.
// ============================================================

static void adsb_reader_task(void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    int r;

    uint8_t dev_addr = s_new_dev_addr;
    ESP_LOGI(TAG, "ADSB reader task started for device %d", dev_addr);
    // Open and configure RTL-SDR
    r = rtlsdr_open(&rtldev, dev_addr,
        driver_obj->mux_protected.device[dev_addr].client_hdl);
    if (r < 0) {
        ESP_LOGE(TAG, "rtlsdr_open failed: %d", r);
        goto done;
    }

    /* Log dongle identification for debugging */
    {
        char model_str[64];
        int force_bt = 0;
        rtlsdr_get_dongle_info(rtldev, model_str, sizeof(model_str), NULL, &force_bt);
        ESP_LOGI(TAG, "RTL-SDR: %s%s", model_str, force_bt ? ", bias-T forced ON" : "");
    }

    r = rtlsdr_set_center_freq(rtldev, 1090000000);
    if (r < 0) fprintf(stderr, "WARNING: Failed to set center freq.\n");
    else       fprintf(stderr, "Tuned to %u Hz.\n", 1090000000);

    r = rtlsdr_set_sample_rate(rtldev, ADSB_SAMPLE_RATE);
    if (r < 0) fprintf(stderr, "WARNING: Failed to set sample rate.\n");
    else       fprintf(stderr, "Sampling at %u S/s.\n", (unsigned)ADSB_SAMPLE_RATE);

    r = rtlsdr_set_tuner_gain_mode(rtldev, 1);  // 1 = manual gain
    if (r != 0) fprintf(stderr, "WARNING: Failed to set tuner gain mode.\n");

    // Initialize gain from settings
    if (g_settings.adsb_gain_mode == 1) {
        // Manual: use saved value
        s_gain_auto = false;
        s_gain_idx = gain_find_idx(g_settings.adsb_gain_tenths);
    } else {
        // Auto: start at max, let adaptive algorithm adjust
        s_gain_auto = true;
        s_gain_idx = R820T_GAIN_COUNT - 1;
    }
    gain_apply();
    fprintf(stderr, "Tuner gain set to %.1f dB (%s).\n",
            R820T_GAINS[s_gain_idx] / 10.0, s_gain_auto ? "adaptive" : "manual");

    // Apply bias-T setting from NVS (default: off)
    r = rtlsdr_set_bias_tee(rtldev, g_settings.adsb_bias_tee ? 1 : 0);
    if (r == 0) ESP_LOGI(TAG, "Bias-T: %s", g_settings.adsb_bias_tee ? "ON" : "OFF");

    r = rtlsdr_reset_buffer(rtldev);
    if (r < 0) fprintf(stderr, "WARNING: Failed to reset buffers.\n");

    vTaskDelay(pdMS_TO_TICKS(100));
    alloc_adsb_transfer();

    // Only mark connected if the bulk transfer buffer is usable.
    // Without it, reads fail on every call — CIT and status bar
    // should reflect the error, not show a green checkmark.
    if (adsb_transfer_ready()) {
        s_rtlsdr_connected = true;
        s_rtlsdr_error = false;
    } else {
        s_rtlsdr_connected = false;
        s_rtlsdr_error = true;  // device enumerated but buffer alloc failed
        ESP_LOGE(TAG, "RTL-SDR init completed but transfer buffer unavailable — reads will fail");
    }

    ESP_LOGI(TAG, "[APP] Free memory: %ld bytes", esp_get_free_heap_size());

    mode_s_init(&state);
    if (ADSB_DEMOD_IS_2400) {
        // The gain loop for this demodulator and the level histogram the
        // decoder fills for it: 0.7 kB of PSRAM, allocated once and kept.
        if (s_gl == NULL) {
            s_gl = heap_caps_calloc(1, sizeof(*s_gl), MALLOC_CAP_SPIRAM);
            if (s_gl) gain_ctl_init(&s_gl->ctl, R820T_GAINS, (int)R820T_GAIN_COUNT, GAIN_FLOOR_IDX);
            else ESP_LOGE(TAG, "gain loop: no memory for its %u bytes - the gain stays where it is", (unsigned)sizeof(*s_gl));
        }
        if (s_gl) {
            memset(s_gl->hist, 0, sizeof(s_gl->hist));
            state.lvl_hist = s_gl->hist;
            s_gl->ctl.idx = s_gain_idx;
            s_gl_req = s_gain_auto ? GL_REQ_RESTART_MAX : GL_REQ_RESTART_KEEP;
            s_gain_warmup = true;
        }
    }
    // Error correction beyond 1-bit and the coherent look: as Kconfig says
    // (ADSB_DECODE_SOFT / _COH / _FIX2 in main/Kconfig.projbuild,
    // docs/DECODER.md section 4), or as the console last asked (`adsb dec`,
    // `adsb coh`). mode_s_init() has just switched them all off, so put
    // them back.
    decoder_apply_options();
    ESP_LOGI(TAG, "mode_s_init done, stack HWM: %u free, heap: %s",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             heap_caps_check_integrity_all(true) ? "OK" : "CORRUPT");

    // Allocate SD log buffer and initialize channel before creating the log
    // file.  sd_log_aircraft() has a lazy-init path, but it re-inits the
    // channel (memset) which would zero out an already-open file handle if
    // sd_log_create() ran first.  Do the allocation here so both paths agree.
    if (!sd_log_buf) {
        sd_log_buf = heap_caps_malloc(SD_LOG_BUFSIZE, MALLOC_CAP_SPIRAM);
        if (sd_log_buf) {
            sd_log_buf_pos = 0;
            sd_log_ch_init(&sd_ch, TAG);
        }
    }
    sd_log_create();

    s_msg_window_start = esp_timer_get_time();

    // ---- Sample path ----
    // This task reads the dongle and does nothing else; the decoder runs
    // in its own task and takes the samples from a ring between the two
    // (usb_stream.h, sample_ring.h). Up to r8 this task read a buffer and
    // then decoded it, and nothing was received while it decoded.
    if (s_ring_mem == NULL) {
        s_ring_mem = heap_caps_aligned_alloc(8, sample_ring_mem_size(ADSB_RING_SLOTS, ADSB_USB_XFER_BYTES), MALLOC_CAP_SPIRAM);
        if (s_ring_mem == NULL) { ESP_LOGE(TAG, "sample ring malloc failed"); goto done; }
        sample_ring_init(&s_ring, s_ring_mem, ADSB_RING_SLOTS, ADSB_USB_XFER_BYTES);
    } else {
        sample_ring_reset(&s_ring);  // no decoder task is running at this point
    }

    // Magnitude buffer for one decoder run, in PSRAM to preserve internal
    // RAM for SDMMC DMA, LVGL, and wallpaper decoding.
    s_mag_buf = heap_caps_malloc(((DEFAULT_BUF_LENGTH / 2) + MODE_S_STREAM_TAIL_MAX) * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    mode_s_stream_reset(&state); // new sample stream: drop any tail carried from a previous run
    if (s_mag_buf == NULL) {
        ESP_LOGE(TAG, "mag buffer malloc failed");
        goto done;
    }

    adsb_log_start();   // log writer task; created once, kept

    if (s_decode_done == NULL) s_decode_done = xSemaphoreCreateBinary();
    if (s_decode_done == NULL) {
        ESP_LOGE(TAG, "decoder semaphore alloc failed");
        free(s_mag_buf);
        s_mag_buf = NULL;
        goto done;
    }
    xSemaphoreTake(s_decode_done, 0);   // clear a stale completion signal
    s_decode_stop = false;
    s_gain_last_eval = esp_timer_get_time();
    if (xTaskCreatePinnedToCoreWithCaps(adsb_decode_task, "adsb_decode", ADSB_DECODE_TASK_STACK, NULL,
                                        ADSB_DECODE_TASK_PRIO, &s_decode_task_hdl, ADSB_DECODE_TASK_CORE,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "decoder task create failed");
        s_decode_task_hdl = NULL;
        free(s_mag_buf);
        s_mag_buf = NULL;
        goto done;
    }

    s_usb_prev_cb_us = 0;
    s_usb_xfers = rtlsdr_stream_start(rtldev, usb_stream_data_cb, NULL);
    if (s_usb_xfers <= 0) {
        ESP_LOGE(TAG, "could not start the USB sample stream");
        s_usb_xfers = 0;
        s_rtlsdr_connected = false;
        s_rtlsdr_error = true;
    } else {
        ESP_LOGI(TAG, "Sample stream: %d USB transfers of %d bytes, ring of %d (%u ms)",
                 s_usb_xfers, ADSB_USB_XFER_BYTES, ADSB_RING_SLOTS,
                 (unsigned)((uint64_t)ADSB_RING_SLOTS * (ADSB_USB_XFER_BYTES / 2) * 1000ULL / ADSB_SAMPLE_RATE));

        // Main loop — runs until stop flag is set (e.g. device removed).
        // Completed transfers are delivered to usb_stream_data_cb() from
        // inside rtlsdr_stream_poll(), and from inside the control
        // transfers of a gain change, which pump the same event queue.
        int64_t  usb_win_start = esp_timer_get_time();
        uint64_t usb_win_bytes0 = s_usb_bytes_total;
        int      silent_windows = 0;
        while (!s_adsb_reader_stop) {
            // Tuner and bias-T writes requested by other tasks: only this
            // task may talk to the device.
            if (s_gain_pending) {
                s_gain_pending = false;
                gain_apply_now();
            }
            if (s_bias_pending >= 0) {
                int on = s_bias_pending;
                s_bias_pending = -1;
                rtlsdr_set_bias_tee(rtldev, on);
                ESP_LOGI(TAG, "Bias-T: %s", on ? "ON" : "OFF");
            }

            rtlsdr_stream_poll(rtldev, pdMS_TO_TICKS(20));

            if (!rtlsdr_stream_alive()) {
                adsb_stream_stats_t us;
                rtlsdr_stream_get_stats(&us);
                ESP_LOGE(TAG, "USB sample stream ended (%lu errors, last status %d) — assuming device disconnected",
                         (unsigned long)us.errors, us.last_error);
                break;
            }

            // What arrived over USB against what the dongle produced, in
            // 5 s windows (adsb_stats_t.usb_capture).
            int64_t now_us = esp_timer_get_time();
            if (now_us - usb_win_start >= 5000000LL) {
                uint64_t got = s_usb_bytes_total - usb_win_bytes0;
                uint64_t produced = (uint64_t)(now_us - usb_win_start) * (uint64_t)ADSB_SAMPLE_RATE * 2ULL / 1000000ULL;
                s_usb_capture_permille = produced ? (uint32_t)(got * 1000ULL / produced) : 0;
                if (got == 0) {
                    if ((silent_windows++ % 12) == 0)
                        ESP_LOGW(TAG, "No samples from the dongle for 5 s");
                } else {
                    silent_windows = 0;
                }
                usb_win_start = now_us;
                usb_win_bytes0 = s_usb_bytes_total;
            }
        }
        rtlsdr_stream_stop(rtldev);
    }

    // Stop the decoder: it finishes the run it is in, then parks.
    s_decode_stop = true;
    xTaskNotifyGive(s_decode_task_hdl);
    if (xSemaphoreTake(s_decode_done, pdMS_TO_TICKS(15000)) == pdTRUE) {
        vTaskDeleteWithCaps(s_decode_task_hdl);   // frees its PSRAM stack
        free(s_mag_buf);
    } else {
        // It may be inside a slow SD write holding the aircraft lock.
        // Deleting it there would leave the lock taken for good, so it is
        // left to park by itself; its buffer stays allocated because it
        // may still be using it.
        ESP_LOGE(TAG, "decoder task did not stop in 15 s — left to finish on its own");
    }
    s_decode_task_hdl = NULL;
    s_mag_buf = NULL;
    s_usb_xfers = 0;

done:
    s_rtlsdr_connected = false;
    s_rtlsdr_error = false;  // device gone, no longer an error — just disconnected
    // Close RTL-SDR device (releases USB interface claims)
    if (rtldev != NULL) {
        rtlsdr_close(rtldev);
        rtldev = NULL;
    }
    // Keep the bulk transfer buffer allocated — it was pre-allocated early
    // in boot when internal RAM was contiguous. Freeing it risks never
    // getting it back due to heap fragmentation.  alloc_adsb_transfer()
    // will see it's already allocated and skip on reconnect.
    sd_log_close();
    // Release USB event pumping back to class_driver_task
    s_reader_owns_events = false;
    ESP_LOGI(TAG, "ADSB reader task exiting");
    s_adsb_reader_task_hdl = NULL;
    vTaskDelete(NULL);
}

// ============================================================
// USB client event callback — now non-blocking
// ============================================================

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;

    switch (event_msg->event)
    {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        xSemaphoreTake(driver_obj->constant.mux_lock, portMAX_DELAY);
        driver_obj->mux_protected.device[event_msg->new_dev.address].dev_addr = event_msg->new_dev.address;
        xSemaphoreGive(driver_obj->constant.mux_lock);

        // Launch the reader task — don't block in the callback.
        // Set the event ownership flag BEFORE creating the task so that
        // class_driver_task sees it immediately when this callback returns,
        // avoiding a window where both tasks pump USB events concurrently.
        s_new_dev_addr = event_msg->new_dev.address;
        s_adsb_reader_stop = false;
        if (s_adsb_reader_task_hdl == NULL) {
            s_reader_owns_events = true;
            xTaskCreate(adsb_reader_task, "adsb_reader", ADSB_READER_TASK_STACK,
                        (void *)driver_obj, ADSB_READER_TASK_PRIO,
                        &s_adsb_reader_task_hdl);
        }
        break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        // Signal the reader task to stop
        s_adsb_reader_stop = true;

        xSemaphoreTake(driver_obj->constant.mux_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
            if (driver_obj->mux_protected.device[i].dev_hdl == event_msg->dev_gone.dev_hdl) {
                driver_obj->mux_protected.device[i].actions = ACTION_CLOSE_DEV;
                driver_obj->mux_protected.flags.unhandled_devices = 1;
            }
        }
        xSemaphoreGive(driver_obj->constant.mux_lock);
        break;

    default:
        ESP_LOGW(TAG, "Unknown USB client event: %d", event_msg->event);
        break;
    }
}

// ============================================================
// Action handlers
// ============================================================

static void action_open_dev(usb_device_t *device_obj)
{
    assert(device_obj->dev_addr != 0);
    ESP_LOGI(TAG, "Opening device at address %d", device_obj->dev_addr);
    ESP_ERROR_CHECK(usb_host_device_open(device_obj->client_hdl, device_obj->dev_addr, &device_obj->dev_hdl));
    device_obj->actions |= ACTION_GET_DEV_INFO;
}

static void action_get_info(usb_device_t *device_obj)
{
    assert(device_obj->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting device information");
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(device_obj->dev_hdl, &dev_info));
    ESP_LOGI(TAG, "\t%s speed", (char *[]){"Low", "Full", "High"}[dev_info.speed]);
    ESP_LOGI(TAG, "\tParent info:");
    if (dev_info.parent.dev_hdl) {
        usb_device_info_t parent_dev_info;
        ESP_ERROR_CHECK(usb_host_device_info(dev_info.parent.dev_hdl, &parent_dev_info));
        ESP_LOGI(TAG, "\t\tBus addr: %d", parent_dev_info.dev_addr);
        ESP_LOGI(TAG, "\t\tPort: %d", dev_info.parent.port_num);
    } else {
        ESP_LOGI(TAG, "\t\tPort: ROOT");
    }
    ESP_LOGI(TAG, "\tbConfigurationValue %d", dev_info.bConfigurationValue);
    device_obj->actions |= ACTION_GET_DEV_DESC;
}

static void action_get_dev_desc(usb_device_t *device_obj)
{
    assert(device_obj->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting device descriptor");
    const usb_device_desc_t *dev_desc;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(device_obj->dev_hdl, &dev_desc));
    usb_print_device_descriptor(dev_desc);
    device_obj->actions |= ACTION_GET_CONFIG_DESC;
}

static void action_get_config_desc(usb_device_t *device_obj)
{
    assert(device_obj->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting config descriptor");
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(device_obj->dev_hdl, &config_desc));
    usb_print_config_descriptor(config_desc, NULL);
    device_obj->actions |= ACTION_GET_STR_DESC;
}

static void action_get_str_desc(usb_device_t *device_obj)
{
    assert(device_obj->dev_hdl != NULL);
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(device_obj->dev_hdl, &dev_info));
    if (dev_info.str_desc_manufacturer) {
        ESP_LOGI(TAG, "Getting Manufacturer string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_manufacturer);
    }
    if (dev_info.str_desc_product) {
        ESP_LOGI(TAG, "Getting Product string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_product);
    }
    if (dev_info.str_desc_serial_num) {
        ESP_LOGI(TAG, "Getting Serial Number string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_serial_num);
    }
}

static void action_close_dev(usb_device_t *device_obj)
{
    if (device_obj->dev_hdl == NULL) return;  // already closed (e.g. by rtlsdr_close)
    ESP_ERROR_CHECK(usb_host_device_close(device_obj->client_hdl, device_obj->dev_hdl));
    device_obj->dev_hdl = NULL;
    device_obj->dev_addr = 0;
}

static void class_driver_device_handle(usb_device_t *device_obj)
{
    uint8_t actions = device_obj->actions;
    device_obj->actions = 0;
    while (actions) {
        if (actions & ACTION_OPEN_DEV)        action_open_dev(device_obj);
        if (actions & ACTION_GET_DEV_INFO)    action_get_info(device_obj);
        if (actions & ACTION_GET_DEV_DESC)    action_get_dev_desc(device_obj);
        if (actions & ACTION_GET_CONFIG_DESC) action_get_config_desc(device_obj);
        if (actions & ACTION_GET_STR_DESC)    action_get_str_desc(device_obj);
        if (actions & ACTION_CLOSE_DEV)       action_close_dev(device_obj);
        actions = device_obj->actions;
        device_obj->actions = 0;
    }
}

// ============================================================
// class_driver_task
// ============================================================

void class_driver_task(void *arg)
{
    class_driver_t driver_obj = {0};
    usb_host_client_handle_t class_driver_client_hdl = NULL;

    ESP_LOGI(TAG, "Registering Client");

    SemaphoreHandle_t mux_lock = xSemaphoreCreateMutex();
    if (mux_lock == NULL) {
        ESP_LOGE(TAG, "Unable to create class driver mutex");
        vTaskSuspend(NULL);
        return;
    }

    // Create aircraft mutex early so display timer and on_msg never see NULL
    if (aircraft_mutex == NULL)
        aircraft_mutex = xSemaphoreCreateMutex();
    rx_pos_init();

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = (void *)&driver_obj,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &class_driver_client_hdl));

    driver_obj.constant.mux_lock = mux_lock;
    driver_obj.constant.client_hdl = class_driver_client_hdl;
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++)
        driver_obj.mux_protected.device[i].client_hdl = class_driver_client_hdl;

    s_driver_obj = &driver_obj;

    while (1) {
        if (driver_obj.mux_protected.flags.unhandled_devices) {
            xSemaphoreTake(driver_obj.constant.mux_lock, portMAX_DELAY);
            for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
                if (driver_obj.mux_protected.device[i].actions)
                    class_driver_device_handle(&driver_obj.mux_protected.device[i]);
            }
            driver_obj.mux_protected.flags.unhandled_devices = 0;
            xSemaphoreGive(driver_obj.constant.mux_lock);
        } else if (driver_obj.mux_protected.flags.shutdown) {
            break;
        } else if (s_reader_owns_events) {
            // Reader task is pumping USB events via bulk/control transfers.
            // We must NOT call usb_host_client_handle_events concurrently.
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            // Use timeout instead of portMAX_DELAY so we stay responsive
            // to device reconnection events (USB host lib may need periodic
            // pumping to complete device enumeration after hot-plug).
            usb_host_client_handle_events(class_driver_client_hdl, pdMS_TO_TICKS(500));
        }
    }

    // If reader task is still running, signal it to stop and wait
    if (s_adsb_reader_task_hdl != NULL) {
        s_adsb_reader_stop = true;
        for (int i = 0; i < 50 && s_adsb_reader_task_hdl != NULL; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Deregistering Class Client");
    ESP_ERROR_CHECK(usb_host_client_deregister(class_driver_client_hdl));
    if (mux_lock) vSemaphoreDelete(mux_lock);
    vTaskSuspend(NULL);
}

void class_driver_client_deregister(void)
{
    xSemaphoreTake(s_driver_obj->constant.mux_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
        if (s_driver_obj->mux_protected.device[i].dev_hdl != NULL) {
            s_driver_obj->mux_protected.device[i].actions |= ACTION_CLOSE_DEV;
            s_driver_obj->mux_protected.flags.unhandled_devices = 1;
        }
    }
    s_driver_obj->mux_protected.flags.shutdown = 1;
    xSemaphoreGive(s_driver_obj->constant.mux_lock);
    ESP_ERROR_CHECK(usb_host_client_unblock(s_driver_obj->constant.client_hdl));
}

int adsb_get_aircraft_for_scope(scope_aircraft_t *out, int max_count)
{
    if (!aircraft_mutex || !out || max_count <= 0) return 0;
    const receiver_pos_t rx = adsb_get_receiver_pos();
    int count = 0;

    // Copy under the lock; range and bearing after it (single precision).
    xSemaphoreTake(aircraft_mutex, portMAX_DELAY);
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE && count < max_count; i++) {
        aircraft_t *a = &aircraft_table[i];
        if (!a->active) continue;
        int64_t age_us = now_us - a->last_seen;
        if (age_us > 180000000LL) continue; // 180s = 3 minutes, matches webapp EXP

        scope_aircraft_t *s = &out[count];
        s->icao = a->icao;
        memcpy(s->callsign, a->callsign, 9);
        s->altitude = a->altitude;
        s->speed = a->speed;
        s->heading = a->heading;
        s->lat = a->lat;
        s->lon = a->lon;
        s->has_position = a->has_position;
        s->vert_rate = a->vert_rate;
        s->msg_count = a->msg_count;
        s->age_ms = (int32_t)(age_us / 1000);
        s->dist_nm = 0;
        s->bearing_deg = 0;
        count++;
    }
    xSemaphoreGive(aircraft_mutex);

    if (rx.fix_valid) {
        for (int i = 0; i < count; i++) {
            scope_aircraft_t *s = &out[i];
            if (!s->has_position) continue;
            float km, brg;
            adsb_gc(rx.lat, rx.lon, s->lat, s->lon, &km, &brg);
            s->dist_nm = km * 0.539957f;
            s->bearing_deg = brg;
        }
    }
    return count;
}
