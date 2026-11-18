#pragma once
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#define MODE_S_ICAO_CACHE_LEN 1024 // Power of two required
#define MODE_S_LONG_MSG_BYTES (112 / 8)
#define MODE_S_UNIT_FEET 0
#define MODE_S_UNIT_METERS 1

// Streaming detection (mode_s_detect_stream): magnitude samples carried
// between chunks = one full frame ((8 us preamble + 112 us payload) at
// 2 samples/us) + 2 samples of look-behind for the out-of-phase test.
// The caller's magnitude buffer must hold MODE_S_STREAM_TAIL + chunk/2.
#define MODE_S_STREAM_TAIL 242

// 2.4 Msps demodulator (mode_s_2400.c). A frame is 120 us = 288 samples;
// MODE_S_2400_SPAN is how far past its first sample the demodulator may read.
#define MODE_S_2400_SPAN 296
#define MODE_S_STREAM_TAIL_2400 (MODE_S_2400_SPAN + 2)
// Largest tail of any demodulator: size magnitude buffers with this.
#define MODE_S_STREAM_TAIL_MAX MODE_S_STREAM_TAIL_2400

// Program state
typedef struct
{
    // Internal state
    uint32_t icao_cache[sizeof(uint32_t) * MODE_S_ICAO_CACHE_LEN * 2]; // Recently seen ICAO addresses cache

    // Configuration
    int fix_errors; // Single bit error correction if true
    int aggressive; // Aggressive detection algorithm
    int check_crc;  // Only display messages with good CRC

    // Statistics for adaptive gain
    volatile uint32_t stat_preambles;  // preambles that passed all checks
    volatile uint32_t stat_crc_ok;     // messages with valid CRC
    volatile uint32_t stat_crc_fail;   // messages with failed CRC

    // Signal quality metrics for adaptive gain (CRC-ok messages only)
    // Preamble signal level: average of 4 preamble peaks (proportional to
    // received power). Range 0–65167. Divide sum by stat_crc_ok for average.
    volatile uint64_t stat_signal_sum;
    // Bit delta: average |mag_high − mag_low| across all bits in message.
    // SNR proxy — higher = cleaner signal. Noise floor at 2550 (10×255).
    // Divide sum by stat_crc_ok for average.
    volatile uint64_t stat_delta_sum;

    // ---- decoder-m1090 extensions -------------------------------------
    // Every field below is 0 after mode_s_init(), and with all of them 0
    // the decoder behaves exactly as it did historically (apart from the
    // DF18/DF24+ frame-length fix, see MODE_S_DF18_FIX in mode-s.c).
    int fix2;                // Exact 2-bit correction for DF17/18 (independent of 'aggressive')
    int strict_es;           // Reject DF17/18 frames whose CA/CF field holds a reserved value
    int fix2_known_icao;     // Accept a 2-bit repair only for an address recently heard error-free
    int pass_df18;           // Deliver DF18 frames to the callback (default: decode and count only)
    uint32_t fix2_min_delta; // Min per-frame bit delta (same scale as the stat_delta_sum terms;
                             // the detector's own floor is 2550) to attempt a 2-bit repair. 0 = ungated
    uint32_t last_delta;        // Set by mode_s_detect(): bit delta of the frame being decoded
    uint32_t last_frame_offset; // Set by mode_s_detect(): index of that frame's preamble in mag[]
    volatile uint32_t stat_fix1;           // frames repaired by 1-bit correction
    volatile uint32_t stat_fix2;           // frames repaired by 2-bit correction
    volatile uint32_t stat_fix2_icao_rej;  // 2-bit repairs refused by fix2_known_icao
    volatile uint32_t stat_strict_rej;     // CRC-ok frames refused by strict_es
    volatile uint32_t stat_df18;           // CRC-ok DF18 frames decoded (delivered or not)
    volatile uint32_t stat_df18_cf[8];     // ... by CF field: 0/1 ADS-B from non-transponder devices
                                           // (ICAO / other address), 2/3/5 TIS-B (fine / coarse / fine
                                           // with non-ICAO address), 4 management, 6 ADS-R, 7 reserved
    volatile uint64_t stat_fail_delta_sum; // sum of bit deltas of CRC-failed frames (noise reference)

    // Streaming state, owned by mode_s_detect_stream()
    uint32_t detect_start;   // mode_s_detect() starts scanning at this index, then clears it
    uint32_t detect_resume;  // set by mode_s_detect(): samples by which the scan overran its limit
    uint32_t stream_tail;    // magnitude samples carried at the front of the stream buffer
    uint64_t stream_samples; // IQ samples consumed since mode_s_stream_reset()
    uint32_t cache_clock_rate; // 0: the ICAO cache ages by wall-clock time (default);
                               // otherwise by the sample count, at this many samples a second
    uint64_t stream_base;    // absolute sample index of mag[0] in the current stream call

    // 2.4 Msps demodulator (mode_s_2400.c). Defaults set by mode_s_init().
    uint16_t d24_gate_x100;  // preamble gate: each pulse pair over the quiet level, x100
    uint16_t d24_gate2_x100; // preamble gate: mean of the four pulse pairs over the quiet level, x100
    uint16_t d24_pre_x100;   // preamble accept: pulse-chip mean over quiet-chip mean, x100
    uint8_t d24_tries;       // sub-sample timing offsets sliced per candidate (1-5)
    uint8_t soft_fix;        // soft rescue: how many least-confident bits it may flip among
                             // (0 = off, max 8). DF17/18 only; honours fix2_known_icao.
    volatile uint32_t stat_soft_fix; // frames repaired by the soft rescue
    uint8_t last_frame_phase; // set by the 2.4 Msps demodulator with last_frame_offset: the frame's
                              // sub-sample timing in fifths of a sample (0-4). 0 at 2.0 Msps.

    // Coherent second look at CRC-failed frames (mode_s_coh.c). Inert unless
    // coh_fix is set AND coh_work points at mode_s_coh_work_size() bytes,
    // and only through mode_s_detect_stream_2400() (it needs the IQ).
    uint8_t coh_fix;         // 1: re-slice CRC-failed frames from IQ
    uint8_t coh_tsearch;     // also try timings this many fifths of a sample either side (0-2; default 1)
    uint8_t coh_df_gate;     // only frames whose first slice read DF17/18: 1 = exactly, 2 = within one
                             // bit; 0 = any. Fewer attempts, fewer frames recovered
    uint16_t coh_gate_x100;  // only frames whose bit delta is at least this multiple (x100) of the
                             // running bit delta of failed candidates; 0 = ungated (default 200)
    void *coh_work;          // working memory, supplied by the application
    uint32_t coh_ref;        // running bit delta of CRC-failed candidates, x16 (the noise reference)
    uint64_t coh_last_tick;  // absolute tick of the last attempt, plus 1 (0 = none)
    const unsigned char *iq_cur; // IQ behind mag[] during a streaming detect call, else NULL
    uint32_t iq_cur_n;       // ... IQ samples in it
    uint32_t iq_tail_n;      // ... magnitude samples in front of it carried from the previous call
    uint32_t iq_tail_valid;  // samples of carried IQ held in coh_work
    float last_freq_hz;      // carrier offset of the last frame the re-slice recovered
    volatile uint32_t stat_coh_gated; // failed candidates that passed the gates
    volatile uint32_t stat_coh_try;   // re-slices run
    volatile uint32_t stat_coh_abort; // ... of which stopped early: no single carrier in the frame
    volatile uint32_t stat_coh_ok;    // frames recovered by the re-slice ...
    volatile uint32_t stat_coh_soft;  // ... of which needed the soft rescue as well
} mode_s_t;

// The struct we use to store information about a decoded message
struct mode_s_msg
{
    // Generic fields
    unsigned char msg[MODE_S_LONG_MSG_BYTES]; // Binary message
    int msgbits;                              // Number of bits in message
    int msgtype;                              // Downlink format #
    int crcok;                                // True if CRC was valid
    uint32_t crc;                             // Message CRC
    int errorbit;                             // Bit corrected. -1 if no bit corrected.
    int aa1, aa2, aa3;                        // ICAO Address bytes 1 2 and 3
    int phase_corrected;                      // True if phase correction was applied.

    // DF 11
    int ca; // Responder capabilities.

    // DF 17
    int metype; // Extended squitter message type.
    int mesub;  // Extended squitter message subtype.
    int heading_is_valid;
    int heading;
    int aircraft_type;
    int fflag;            // 1 = Odd, 0 = Even CPR message.
    int tflag;            // UTC synchronized?
    int raw_latitude;     // Non decoded latitude
    int raw_longitude;    // Non decoded longitude
    char flight[9];       // 8 chars flight number.
    int ew_dir;           // 0 = East, 1 = West.
    int ew_velocity;      // E/W velocity.
    int ns_dir;           // 0 = North, 1 = South.
    int ns_velocity;      // N/S velocity.
    int vert_rate_source; // Vertical rate source.
    int vert_rate_sign;   // Vertical rate sign.
    int vert_rate;        // Vertical rate.
    int velocity;         // Computed from EW and NS velocity.

    // DF4, DF5, DF20, DF21
    int fs;       // Flight status for DF4,5,20,21
    int dr;       // Request extraction of downlink request.
    int um;       // Request extraction of downlink request.
    int identity; // 13 bits identity (Squawk).

    // Fields used by multiple message types.
    int altitude, unit;
};

typedef void (*mode_s_callback_t)(mode_s_t *self, struct mode_s_msg *mm);

void mode_s_init(mode_s_t *self);
void mode_s_compute_magnitude_vector(unsigned char *data, uint16_t *mag, uint32_t size);
void mode_s_detect(mode_s_t *self, uint16_t *mag, uint32_t maglen, mode_s_callback_t);
void mode_s_decode(mode_s_t *self, struct mode_s_msg *mm, unsigned char *msg);

// CRC-24 over the whole frame (data + parity), one table step per byte.
// 0 exactly when the frame is valid; otherwise the error syndrome.
uint32_t mode_s_crc_syndrome(const unsigned char *msg, int bits);

// Streaming front end. Feed successive chunks of raw IQ (interleaved
// unsigned 8-bit I,Q). Unlike calling mode_s_compute_magnitude_vector() +
// mode_s_detect() per chunk, frames that straddle a chunk boundary are
// decoded, and the result is identical to processing the whole stream as
// one buffer. 'mag' must persist between calls and hold at least
// MODE_S_STREAM_TAIL + iq_bytes/2 samples. Call mode_s_stream_reset()
// whenever the sample stream is discontinuous (start, dongle restart).
void mode_s_stream_reset(mode_s_t *self);
void mode_s_detect_stream(mode_s_t *self, unsigned char *iq, uint32_t iq_bytes,
                          uint16_t *mag, mode_s_callback_t cb);

// How the cache of recently seen ICAO addresses tells time. Normally by
// the wall clock (sample_rate_hz = 0). When a recording is replayed faster
// or slower than it was made, the wall clock is the wrong clock: pass the
// recording's sample rate and the cache ages by samples consumed instead,
// which also makes a replay give the same result every time. The cache is
// emptied on every call.
void mode_s_cache_clock(mode_s_t *self, uint32_t sample_rate_hz);

// The sample stream lost samples at this point (the receiver was not
// reading, or a buffer overflowed). The carried tail is dropped, so no
// frame is assembled from samples that were not adjacent on air, and the
// sample count moves on by lost_samples, so the times of later frames stay
// on the receiver's sample clock. Pass 0 if the number lost is not known.
// The frames decoded are exactly those of decoding the two sides of the
// loss as separate streams (harness/check_stream.py proves it).
void mode_s_stream_gap(mode_s_t *self, uint64_t lost_samples);

// The same for IQ sampled at 2.4 Msps (mode_s_2400.c). 'mag' must hold
// MODE_S_STREAM_TAIL_2400 + iq_bytes/2 samples. mode_s_detect_2400() is the
// one-shot form, the counterpart of mode_s_detect().
void mode_s_detect_2400(mode_s_t *self, uint16_t *mag, uint32_t maglen, mode_s_callback_t cb);
void mode_s_detect_stream_2400(mode_s_t *self, unsigned char *iq, uint32_t iq_bytes,
                               uint16_t *mag, mode_s_callback_t cb);

// Coherent second look (mode_s_coh.c). The application allocates
// mode_s_coh_work_size() bytes (any memory; PSRAM on the device), points
// coh_work at them and sets coh_fix. mode_s_coh_reslice() re-slices the
// 112-bit frame whose preamble starts at mag[n] + f/5 of the current
// streaming call: msg/conf receive bits and per-bit confidence, *dt_out the
// timing correction it chose (fifths of a sample), *freq_hz the carrier
// offset. Returns 1 if it sliced the frame, 0 if the IQ is not available,
// -1 if the frame shows no single carrier (not worth slicing).
size_t mode_s_coh_work_size(void);
int mode_s_coh_reslice(mode_s_t *self, uint32_t n, int f, unsigned char *msg, uint32_t *conf,
                       int *dt_out, float *freq_hz);
void mode_s_coh_stream_begin(mode_s_t *self, const unsigned char *iq, uint32_t n, uint32_t tail);
void mode_s_coh_stream_end(mode_s_t *self, const unsigned char *iq, uint32_t n, uint32_t tail,
                           uint32_t keep_from, uint32_t total);

// Shared with mode_s_2400.c
int mode_s_msg_len_by_type(int type);
int icao_addr_was_recently_seen(mode_s_t *self, uint32_t addr);

// Absolute sample index (counted from mode_s_stream_reset) of the preamble
// of the frame being delivered. Valid inside the callback for frames that
// arrive via mode_s_detect_stream(). This is the time-of-arrival hook.
static inline uint64_t mode_s_frame_sample_index(const mode_s_t *self)
{
    return self->stream_base + self->last_frame_offset;
}

// The same instant in 12 MHz ticks (2.4 Msps demodulator: 5 ticks per
// sample plus the sub-sample timing). This is the clock Beast-format
// timestamps use; here it marks the START of the preamble, in the
// receiver's own sample clock, counted from mode_s_stream_reset().
static inline uint64_t mode_s_frame_ticks_12mhz(const mode_s_t *self)
{
    return mode_s_frame_sample_index(self) * 5u + self->last_frame_phase;
}
void runme();