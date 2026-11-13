// mqtt_batch.h – per-aircraft coalescing and size-bounded JSON batches for
// the MQTT feeder.
//
// Why this exists. The feeder used to queue one entry per decoded MESSAGE
// into a 64-entry buffer that was flushed every 2 s. Above 32 messages per
// second the buffer filled early in each 2 s window and every later
// message was dropped until the next flush. The busier the sky, the
// shorter the slice of each window that was uplinked and the fewer
// aircraft it covered, with the strong, chatty ones crowding out the rest
// (harness/feeder/coverage_sim.py puts numbers on it).
//
// Here each aircraft has one slot holding its latest state, so a flush
// carries every aircraft heard since the previous flush exactly once, and
// the batch is serialised as one or more JSON messages, each bounded in
// bytes.
//
// Pure C with no ESP-IDF dependencies: built and tested on a PC by
// harness/feeder/.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mqtt_feeder.h" // mqtt_aircraft_t

#ifdef __cplusplus
extern "C" {
#endif

// One slot per aircraft the decoder can track at once. Must not be smaller
// than ADSB_MAX_AIRCRAFT in class_driver.h; class_driver.c checks it at
// compile time. (Kept as its own number so this header stays free of
// firmware headers and builds on a PC.)
#define MQTT_BATCH_SLOTS 256

// Room for the JSON text of one aircraft entry. The longest possible entry
// is 241 characters: every field present at its widest and all 16
// characters of ICAO and callsign needing a \uXXXX escape
// (harness/feeder/test_batch.c builds it). A typical entry is 110 to 140.
// A message buffer must hold at least one entry plus the envelope.
#define MQTT_BATCH_ENTRY_MAX 256
#define MQTT_BATCH_MSG_MIN (MQTT_BATCH_ENTRY_MAX + 64)

typedef struct {
    mqtt_aircraft_t slot[MQTT_BATCH_SLOTS];
    uint64_t key[MQTT_BATCH_SLOTS]; // the slot's ICAO text packed into an integer
    int count;                      // slots in use since the last reset

    // Counters, never reset by mqtt_batch_reset()
    uint32_t updates;      // calls to mqtt_batch_put()
    uint32_t coalesced;    // ... that replaced an older state of the same aircraft
    uint32_t dropped_full; // ... dropped because every slot was taken
} mqtt_batch_t;

// Zero everything, counters included.
void mqtt_batch_init(mqtt_batch_t *b);

// Empty the slots after a flush. Counters are kept.
void mqtt_batch_reset(mqtt_batch_t *b);

// Record the latest state of one aircraft. Returns false only if the
// aircraft is new to this batch and all slots are taken. Cost: one pass
// over at most MQTT_BATCH_SLOTS 64-bit keys.
bool mqtt_batch_put(mqtt_batch_t *b, const mqtt_aircraft_t *ac);

// Copy the collected aircraft to out (room for max entries) and empty the
// slots. Returns the number copied. Entries beyond max are discarded, which
// cannot happen when max >= MQTT_BATCH_SLOTS.
int mqtt_batch_take(mqtt_batch_t *b, mqtt_aircraft_t *out, int max);

// Serialise list[*next .. count) into buf as ONE complete JSON message
//
//     {"aircraft":[{...},{...}],"rx_lat":..,"rx_lon":..}
//
// taking as many entries as fit in cap bytes (terminating NUL included),
// and advance *next past them. The receiver position is included when
// have_rx is set and the coordinates are valid.
//
// Returns the message length, or 0 if no message was produced: either
// nothing was left, or cap is below MQTT_BATCH_MSG_MIN. Call repeatedly
// until *next == count. *next always advances when it is below count and
// cap is large enough, so the loop terminates.
//
// Field names, order and presence rules are those the feeder has always
// used. Numbers are rounded to what they mean: positions to 6 decimals
// (0.11 m), distance to 2 (18 m). An aircraft whose position is not a
// valid coordinate is sent without one.
size_t mqtt_batch_serialise(const mqtt_aircraft_t *list, int count, int *next,
                            bool have_rx, double rx_lat, double rx_lon,
                            char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
