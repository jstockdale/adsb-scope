/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "rtl-sdr.h"
#include "mode-s.h"
#include "lvgl.h"


#define CLIENT_NUM_EVENT_MSG 5

#define MAX_PACKET_SIZE 16384
#define DEFAULT_BUF_LENGTH (MAX_PACKET_SIZE * 8)


typedef enum
{
    ACTION_OPEN_DEV = (1 << 0),
    ACTION_GET_DEV_INFO = (1 << 1),
    ACTION_GET_DEV_DESC = (1 << 2),
    ACTION_GET_CONFIG_DESC = (1 << 3),
    ACTION_GET_STR_DESC = (1 << 4),
    ACTION_CLOSE_DEV = (1 << 5),
} action_t;

#define DEV_MAX_COUNT 128

typedef struct
{
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    action_t actions;
} usb_device_t;

typedef struct
{
    struct
    {
        union
        {
            struct
            {
                uint8_t unhandled_devices : 1; /**< Device has unhandled devices */
                uint8_t shutdown : 1;          /**<  */
                uint8_t reserved6 : 6;         /**< Reserved */
            };
            uint8_t val;                    /**< Class drivers' flags value */
        } flags;                            /**< Class drivers' flags */
        usb_device_t device[DEV_MAX_COUNT]; /**< Class drivers' static array of devices */
    } mux_protected;                        /**< Mutex protected members. Must be protected by the Class mux_lock when accessed */

    struct
    {
        usb_host_client_handle_t client_hdl;
        SemaphoreHandle_t mux_lock; /**< Mutex for protected members */
    } constant;                     /**< Constant members. Do not change after installation thus do not require a critical section or mutex */
} class_driver_t;

static const char *TAG = "CLASS";
static rtlsdr_dev_t *rtldev = NULL;
static class_driver_t *s_driver_obj;
static mode_s_t state;

void on_msg(mode_s_t *self, struct mode_s_msg *mm);

void demodulate(uint8_t *source, int length);

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);

static void action_open_dev(usb_device_t *device_obj);

static void action_get_info(usb_device_t *device_obj);

static void action_get_dev_desc(usb_device_t *device_obj);

static void action_get_config_desc(usb_device_t *device_obj);

static void action_get_str_desc(usb_device_t *device_obj);

static void action_close_dev(usb_device_t *device_obj);

static void class_driver_device_handle(usb_device_t *device_obj);

#ifdef __cplusplus
extern "C" {
#endif

extern void class_driver_task(void *arg);

extern void class_driver_client_deregister(void);

extern void alloc_adsb_transfer(void);

extern void adsb_update_display(lv_obj_t *table);
extern void sd_log_rename_with_time(void);
extern void sd_log_close(void);
extern void sd_log_print_status(void);
extern bool sd_log_is_active(void);
extern void sd_log_create_new(void);
extern void sd_clear_dirty_flag(void);

// Receiver position — updated by GPS task, read by ADS-B display/logging
typedef struct {
    double lat;         // decimal degrees, positive = N
    double lon;         // decimal degrees, positive = E
    double alt_m;       // altitude in meters (from GGA, 0 if unavailable)
    int    fix_valid;   // nonzero when we have a valid fix
    int    sats;        // satellite count from GGA
    double hdop;        // horizontal dilution of precision
    int    fix_quality; // GGA fix quality (0=none, 1=GPS, 2=DGPS)
} receiver_pos_t;

extern void adsb_set_receiver_pos(double lat, double lon, double alt_m,
                                  int sats, double hdop, int fix_quality);
extern receiver_pos_t adsb_get_receiver_pos(void);

// ADS-B statistics — updated by on_msg, read by CIT test / status bar
typedef struct {
    uint32_t total_messages;
    float    msg_rate;          // messages per second
    int      active_aircraft;   // aircraft seen within expiry window
    bool     rtlsdr_connected;  // true while reader task is running AND transfer buffer OK
    bool     rtlsdr_error;      // true if device seen but transfer buffer alloc failed
    uint32_t nearest_icao;      // ICAO of nearest aircraft (0 if none)
    double   nearest_dist_nm;   // distance to nearest in nautical miles
    int      nearest_alt;       // altitude of nearest aircraft
    char     nearest_callsign[9]; // callsign of nearest (empty string if none)
    double   farthest_dist_nm;  // distance to farthest aircraft in nautical miles
    uint32_t farthest_icao;     // ICAO of farthest aircraft (0 if none)
    uint16_t gain_tenths;       // current tuner gain in tenths of dB (e.g. 496 = 49.6 dB)
    bool     gain_auto;         // true if using adaptive gain
    int      gain_phase;        // 1 = fast convergence, 2 = steady-state
    float    crc_error_rate;    // CRC failure rate (0.0 – 1.0) over last evaluation window
    float    pos_rate;          // position updates per second (EMA in phase 2)
    uint32_t avg_signal;        // avg preamble signal level of CRC-ok messages (0–65167)
    uint32_t avg_delta;         // avg bit delta / SNR proxy of CRC-ok messages (noise floor ~2550)
    char     dongle_model[64];  // human-readable RTL-SDR dongle description (e.g. "RTL-SDR Blog V4 (R828D)")
    // DF18: extended squitter from non-transponder sources. Decoded and
    // counted since the receiver started; not shown as aircraft.
    uint32_t df18_total;        // all CRC-ok DF18 frames
    uint32_t df18_last_min;     // ... in the most recent complete 60 s window
    uint32_t df18_tisb;         // CF 2/3/5: TIS-B (radar targets relayed by ground stations)
    uint32_t df18_adsr;         // CF 6: ADS-R (ground rebroadcast of traffic on another link)
    uint32_t df18_adsb_nt;      // CF 0/1: ADS-B from non-transponder devices
    uint32_t df18_other;        // CF 4 (TIS-B/ADS-R management) and CF 7 (reserved)
    float    decode_load;       // share of one core spent in the decoder, 0.0-1.0: wall time
                                // inside demodulate() over the last ~5 s (includes any time the
                                // decoder task was preempted there, so it is an upper bound).
                                // The decoder keeps up with the dongle as long as this stays
                                // below 1.0.
    float    sdlog_load;        // ... of which in the SD log
    float    peak_decode_load;  // highest decode_load of any window since boot
    // How much of the air time reaches the decoder. Samples flow
    //   dongle -> USB transfers -> ring (PSRAM) -> decoder
    // and can be lost in two places: at the dongle, if no USB transfer is
    // pending (usb_capture below 1.0, usb_starved counting the events), and
    // at the ring, if the decoder falls behind by more than it holds
    // (ring_dropped). capture is what is left after both.
    float    capture;           // sample bytes decoded / produced at the sample rate, last ~5 s
    float    usb_capture;       // sample bytes received over USB / produced, last ~5 s
    float    min_capture;       // lowest capture of any window since boot
    uint32_t usb_xfers;         // USB transfers kept queued (ADSB_USB_XFER_COUNT unless memory was short)
    uint32_t usb_starved;       // times the endpoint was found with no transfer pending, since boot
    uint32_t ring_fill_pct;     // ring in use now
    uint32_t ring_max_pct;      // ... and the most it has ever been
    uint32_t ring_dropped;      // USB transfers dropped because the ring was full, since boot
    uint32_t stream_gaps;       // losses the decoder was told about (it restarts cleanly after each)
    // The log: one line per message, written by its own task. When the
    // console cannot take them all, lines are dropped, not samples.
    uint32_t log_lines_s;       // lines written per second, last ~5 s
    uint32_t log_bytes_s;       // ... and bytes, line endings included
    uint32_t log_dropped;       // lines dropped since boot
    uint32_t log_dropped_win;   // ... of which in the last ~5 s
    // The extremes above (peak_decode_load, min_capture) and the three
    // counts below run from the last adsb_stats_mark(), or from boot.
    uint32_t mark_age_s;        // seconds since the mark
    uint32_t ring_dropped_mark; // transfers dropped by the ring since the mark
    uint32_t usb_starved_mark;  // endpoint found dry since the mark
    uint32_t log_dropped_mark;  // log lines dropped since the mark
} adsb_stats_t;

// Start a fresh observation period: the extremes and the "since the mark"
// counts in adsb_stats_t restart from now. Used by `diag mark`, and after a
// replay, which distorts them.
void adsb_stats_mark(void);

// ---- Replay: a recorded sample file decoded on the device ----
// The file is raw unsigned 8-bit I/Q at ADSB_SAMPLE_RATE (what `rtl_sdr`
// writes). It is fed through the sample ring to the decoder and tracker in
// place of the dongle, as fast as the SD card delivers it and never faster
// than the decoder takes it, so nothing is dropped and the same file gives
// the same result every time. While it runs: live samples are discarded,
// nothing is sent to MQTT or written to the SD log or the serial log, the
// adaptive gain loop is paused, and the aircraft on screen are the
// recording's. The receiver must be running (dongle attached): the replay
// borrows its decoder.
typedef struct {
    bool     running;
    bool     done;            // a replay has finished (see ok / error)
    bool     ok;
    char     path[128];
    char     error[64];
    uint64_t file_bytes;
    uint64_t fed_bytes;
    uint32_t signal_ms;       // length of what was fed, at the sample rate
    uint32_t wall_ms;         // how long the replay took
    uint32_t decode_busy_ms;  // time spent inside the decoder for it
    uint32_t messages;        // messages decoded
    uint32_t ring_dropped;    // must be 0
    char     decoder[96];     // the decoder options it ran with (adsb_decoder_describe)
    uint32_t fix1, fix2, soft;            // frames repaired during the replay, by kind
    uint32_t coh_try, coh_abort, coh_ok;  // coherent look: re-slices, stopped early, frames recovered
} adsb_replay_status_t;

bool adsb_replay_start(const char *path, char *err, size_t err_len);
void adsb_replay_stop(void);
void adsb_replay_get(adsb_replay_status_t *out);

extern adsb_stats_t adsb_get_stats(void);

// ---- Decoder options, switchable while the receiver runs ----
// Console: `adsb dec`, `adsb coh`. What they are and what each costs and
// risks: docs/DECODER.md sections 4 and 5.7. Boot values come from Kconfig
// (ADSB_DECODE_FIX2, ADSB_DECODE_SOFT, ADSB_DECODE_COH); nothing is stored.
// A change is taken over by the decoder task between two of its runs, so
// the decoder never sees one in mid-run.
typedef struct {
    bool    fix2;        // exact 2-bit CRC correction (DF17/18)
    bool    strict;      // refuse extended squitters with a reserved CA/CF value
    bool    known_addr;  // 2-bit and soft repairs only for an address heard error-free in the last 60 s
    uint8_t soft;        // soft rescue: this many least confident bits considered (0 = off, 8 = usual)
    bool    coh;         // coherent second look at frames that fail their CRC (2.4 Msps only)
} adsb_decoder_opts_t;

typedef struct {
    bool     applied;      // the decoder runs with the options asked for (false: receiver stopped, or not yet taken over)
    bool     coh_active;   // the coherent look is really on (it needs the 2.4 Msps demodulator and 9.6 kB of PSRAM)
    uint32_t fix1, fix2, soft;        // frames repaired since the receiver started: 1-bit, 2-bit, soft rescue
    uint32_t addr_refused;            // 2-bit repairs refused by the known-address gate
    uint32_t strict_refused;          // CRC-ok frames refused by the strict check
    uint32_t coh_try;                 // coherent look: re-slices run ...
    uint32_t coh_abort;               // ... of which stopped early (no single carrier)
    uint32_t coh_ok;                  // ... frames it recovered
    uint32_t coh_soft;                // ... of which with the soft rescue as well
    uint32_t stack_free;              // least free stack the decoder task has had, bytes (0 = not running)
} adsb_decoder_info_t;

void adsb_decoder_get(adsb_decoder_opts_t *opts, adsb_decoder_info_t *info);  // either may be NULL
void adsb_decoder_set(const adsb_decoder_opts_t *opts);
// One line saying what is on, e.g. "1-bit fix, soft rescue (known address, strict), coherent look".
void adsb_decoder_describe(const adsb_decoder_opts_t *opts, char *buf, size_t len);

// Set tuner gain: mode 0=auto (adaptive), mode 1=manual at gain_tenths
extern void adsb_set_gain(int mode, uint16_t gain_tenths);

// Restart adaptive gain from current level — re-enters P1 without resetting to max.
extern void adsb_gain_restart(void);

// Sort column for on-device aircraft list
typedef enum {
    ADSB_SORT_DIST = 0,  // default
    ADSB_SORT_ICAO,
    ADSB_SORT_CALL,
    ADSB_SORT_ALT,
    ADSB_SORT_SPD,
    ADSB_SORT_HDG,
} adsb_sort_col_t;

extern void adsb_set_sort(int col, bool ascending);

// Format aircraft list into text buffer, sorted by current sort column.
// Returns number of aircraft written.
extern int adsb_format_aircraft_list(char *buf, int bufsize);

// Pre-allocate USB transfer buffers — call after usb_host_install(),
// before class_driver_task starts.  Idempotent (safe to call from rtlsdr_open too).
extern void init_adsb_dev(void);

// Check if USB bulk transfer buffer is allocated and ready
extern bool adsb_transfer_ready(void);

// Enable/disable bias-T power on RTL-SDR antenna port (4.5V DC).
// Safe to call anytime — no-op if RTL-SDR not connected.
extern void adsb_set_bias_tee(bool on);

// Free bulk transfer on disconnect — allows re-alloc on reconnect
extern void free_adsb_transfer(void);

// How many aircraft the device handles at a time. One number for the whole
// chain – tracked by the decoder, drawn on the scope, listed in the ADS-B
// app, uplinked in one MQTT batch – so that no stage silently shows or
// sends fewer than the one before it.
#define ADSB_MAX_AIRCRAFT 256

// Aircraft data for scope radar display
#define MAX_SCOPE_AIRCRAFT ADSB_MAX_AIRCRAFT

// Text buffer for adsb_format_aircraft_list(): one row of ADSB_LIST_ROW_LEN
// characters per aircraft, plus the margin the formatter keeps free.
#define ADSB_LIST_ROW_LEN  37
#define ADSB_LIST_BUF_SIZE (ADSB_MAX_AIRCRAFT * ADSB_LIST_ROW_LEN + 128)

typedef struct {
    uint32_t icao;
    char callsign[9];
    int altitude;
    int speed;
    int heading;
    int vert_rate;
    double lat, lon;
    int has_position;
    double dist_nm;
    double bearing_deg;
    uint32_t msg_count;
    int32_t age_ms;      // milliseconds since last seen
} scope_aircraft_t;

// Get active aircraft with computed distance/bearing from receiver.
// Returns number of aircraft copied.
extern int adsb_get_aircraft_for_scope(scope_aircraft_t *out, int max_count);

// Persistent scope trail recording — call after ADS-B init, records at 1Hz from boot.
extern void scope_trail_init(void);

#ifdef __cplusplus
}
#endif