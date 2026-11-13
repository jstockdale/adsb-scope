// sample_ring.h – the queue between the USB reader and the decoder.
//
// Why this exists. Up to r8 one task read a buffer of samples from the
// dongle and then decoded it; nothing was read while it decoded, so that
// air time was lost (docs/UPLINK.md 2.2). Now a reader task does nothing
// but move each finished USB transfer into this ring, and the decoder
// takes samples out of it at its own pace.
//
// Shape: a fixed number of equal slots, one USB transfer per slot, plus a
// small record per slot. One producer and one consumer, on different
// tasks, with no lock: the producer alone moves 'head', the consumer alone
// moves 'tail'. A slot stays owned by the consumer until it is released,
// so samples can be decoded in place, without another copy.
//
// When the ring is full the producer drops the transfer, counts it, and
// marks the next stored slot as following a gap of that many samples. The
// reader can also declare samples lost upstream (the dongle was not being
// read). Either way the decoder is told, so it never joins samples that
// were not adjacent on air and its sample count stays true.
//
// Pure C11 with no ESP-IDF dependencies: tested on a PC (harness/ring/).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t len;   // bytes stored in the slot
    uint32_t lost;  // IQ samples lost immediately before this slot
    uint8_t  gap;   // 1: the stream is discontinuous before this slot
    int64_t  t_us;  // when the transfer was handed over (caller's clock)
} sample_ring_rec_t;

typedef struct {
    uint8_t *data;            // slots * slot_bytes
    sample_ring_rec_t *rec;   // one per slot
    uint32_t slots;           // one slot is always left empty
    uint32_t slot_bytes;
    atomic_uint head;         // next slot to write   (producer)
    atomic_uint tail;         // next slot to read    (consumer)

    // Producer side
    uint64_t pending_lost;    // samples lost since the last stored slot
    bool     pending_gap;     // a gap to report on the next stored slot

    // Counters (written by the producer; read by anyone, unlocked)
    uint32_t stored;          // transfers stored
    uint32_t dropped;         // transfers dropped because the ring was full
    uint64_t dropped_bytes;
    uint32_t gaps;            // gaps reported to the consumer
    uint32_t max_fill;        // most slots ever in use
} sample_ring_t;

// Bytes of memory sample_ring_init() needs for this geometry.
size_t sample_ring_mem_size(uint32_t slots, uint32_t slot_bytes);

// Lay the ring out in 'mem' (sample_ring_mem_size() bytes, 8-byte
// aligned). slots must be at least 2; slot_bytes even and non-zero.
// Returns false if the arguments are unusable.
bool sample_ring_init(sample_ring_t *r, void *mem, uint32_t slots, uint32_t slot_bytes);

// Empty the ring and forget pending gaps; counters are kept. Only when
// neither side is running.
void sample_ring_reset(sample_ring_t *r);

// ---- producer ----

// Store one transfer (len <= slot_bytes; an odd length loses its last
// byte and is followed by a gap). Returns false if it was dropped because
// the ring was full.
bool sample_ring_put(sample_ring_t *r, const uint8_t *src, uint32_t len, int64_t t_us);

// Samples were lost before the next transfer (0 = a break of unknown size).
void sample_ring_note_loss(sample_ring_t *r, uint32_t samples);

// ---- consumer ----

// The longest run of stored transfers, starting at the oldest, that is
// contiguous in memory and on air and no longer than max_bytes: a run
// stops before a slot marked as following a gap, after a slot that is not
// full, and at the end of the buffer. Returns the number of slots (0 if
// the ring is empty) and describes the run:
//   *data, *bytes   the samples, to be used in place until released
//   *gap, *lost     whether the stream is discontinuous BEFORE this run,
//                   and by how many samples (0 = unknown)
//   *t_us           the first slot's time stamp
// max_bytes below the first slot's length still returns that one slot.
uint32_t sample_ring_peek(sample_ring_t *r, uint32_t max_bytes,
                          const uint8_t **data, uint32_t *bytes,
                          bool *gap, uint32_t *lost, int64_t *t_us);

// Give back the first n slots (n as returned by sample_ring_peek()).
void sample_ring_release(sample_ring_t *r, uint32_t n);

// ---- either side ----

// Slots in use right now.
uint32_t sample_ring_fill(const sample_ring_t *r);

#ifdef __cplusplus
}
#endif
