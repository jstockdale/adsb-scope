// mqtt_batch.c – see mqtt_batch.h.

#include "mqtt_batch.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void mqtt_batch_init(mqtt_batch_t *b)
{
    memset(b, 0, sizeof(*b));
}

void mqtt_batch_reset(mqtt_batch_t *b)
{
    b->count = 0;
}

// The ICAO text (up to 8 characters) packed into one integer: an exact key,
// two different strings never share one. Stops at the terminating NUL, so
// whatever follows it in the caller's buffer does not matter.
static uint64_t icao_key(const char *s)
{
    uint64_t k = 0;
    int i;
    for (i = 0; i < 8 && s[i]; i++)
        k = (k << 8) | (unsigned char)s[i];
    return k;
}

bool mqtt_batch_put(mqtt_batch_t *b, const mqtt_aircraft_t *ac)
{
    const uint64_t k = icao_key(ac->icao);
    int i;
    b->updates++;
    for (i = 0; i < b->count; i++)
    {
        if (b->key[i] == k)
        {
            b->slot[i] = *ac;
            b->coalesced++;
            return true;
        }
    }
    if (b->count >= MQTT_BATCH_SLOTS)
    {
        b->dropped_full++;
        return false;
    }
    b->key[b->count] = k;
    b->slot[b->count] = *ac;
    b->count++;
    return true;
}

int mqtt_batch_take(mqtt_batch_t *b, mqtt_aircraft_t *out, int max)
{
    int n = b->count;
    if (n > max)
        n = max;
    if (n > 0)
        memcpy(out, b->slot, (size_t)n * sizeof(out[0]));
    b->count = 0;
    return n;
}

// ---- JSON serialisation ---------------------------------------------------

// Bounded writer: once it overflows it stays overflowed and writes nothing.
typedef struct {
    char *p;
    size_t cap; // including the terminating NUL
    size_t len;
    bool overflow;
} wr_t;

static void wr_raw(wr_t *w, const char *s, size_t n)
{
    if (w->overflow || w->len + n + 1 > w->cap)
    {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->len, s, n);
    w->len += n;
    w->p[w->len] = '\0';
}

static void wr_str(wr_t *w, const char *s)
{
    wr_raw(w, s, strlen(s));
}

static void wr_int(wr_t *w, long v)
{
    char t[24];
    int n = snprintf(t, sizeof(t), "%ld", v);
    if (n > 0)
        wr_raw(w, t, (size_t)n);
}

static void wr_fixed(wr_t *w, double v, int decimals)
{
    char t[40];
    int n = snprintf(t, sizeof(t), "%.*f", decimals, v);
    if (n > 0 && (size_t)n < sizeof(t))
        wr_raw(w, t, (size_t)n);
    else
        w->overflow = true;
}

// A JSON string: quotes, with anything outside printable ASCII escaped.
// Reads at most max bytes of s.
static void wr_json_string(wr_t *w, const char *s, size_t max)
{
    size_t i;
    wr_raw(w, "\"", 1);
    for (i = 0; i < max && s[i]; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
        {
            char t[2] = {'\\', (char)c};
            wr_raw(w, t, 2);
        }
        else if (c < 0x20 || c >= 0x7F)
        {
            char t[8];
            snprintf(t, sizeof(t), "\\u%04x", (unsigned)c);
            wr_raw(w, t, 6);
        }
        else
        {
            wr_raw(w, (const char *)&s[i], 1);
        }
    }
    wr_raw(w, "\"", 1);
}

static bool valid_lat_lon(double lat, double lon)
{
    return isfinite(lat) && isfinite(lon) &&
           lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

// One aircraft object. Same keys, order and presence rules as the feeder
// has always used.
static void wr_entry(wr_t *w, const mqtt_aircraft_t *ac)
{
    wr_str(w, "{\"icao\":");
    wr_json_string(w, ac->icao, sizeof(ac->icao) - 1);
    if (ac->callsign[0])
    {
        wr_str(w, ",\"callsign\":");
        wr_json_string(w, ac->callsign, sizeof(ac->callsign) - 1);
    }
    if (ac->has_pos && valid_lat_lon(ac->lat, ac->lon))
    {
        wr_str(w, ",\"lat\":");
        wr_fixed(w, ac->lat, 6);
        wr_str(w, ",\"lon\":");
        wr_fixed(w, ac->lon, 6);
    }
    if (ac->alt_ft != 0)
    {
        wr_str(w, ",\"alt\":");
        wr_int(w, ac->alt_ft);
    }
    if (ac->speed_kt != 0)
    {
        wr_str(w, ",\"speed\":");
        wr_int(w, ac->speed_kt);
    }
    if (ac->heading_deg >= 0)
    {
        wr_str(w, ",\"hdg\":");
        wr_int(w, ac->heading_deg);
    }
    if (ac->vert_rate_fpm != 0)
    {
        wr_str(w, ",\"vr\":");
        wr_int(w, ac->vert_rate_fpm);
    }
    // No two points on Earth are further apart than about 10,800 nm; the
    // upper bound only keeps a corrupt value from producing a huge number.
    if (ac->dist_nm > 0 && isfinite(ac->dist_nm) && ac->dist_nm < 100000.0f)
    {
        wr_str(w, ",\"dist\":");
        wr_fixed(w, (double)ac->dist_nm, 2);
    }
    if (ac->bearing_deg >= 0)
    {
        wr_str(w, ",\"brg\":");
        wr_int(w, ac->bearing_deg);
    }
    wr_str(w, "}");
}

size_t mqtt_batch_serialise(const mqtt_aircraft_t *list, int count, int *next,
                            bool have_rx, double rx_lat, double rx_lon,
                            char *buf, size_t cap)
{
    char tail_buf[64];
    wr_t tail = {tail_buf, sizeof(tail_buf), 0, false};
    wr_t msg = {buf, cap, 0, false};
    int taken = 0;

    if (!list || !next || !buf || *next >= count || cap < MQTT_BATCH_MSG_MIN)
        return 0;

    // The closing part is built first so its length is known while filling.
    tail_buf[0] = '\0';
    wr_str(&tail, "]");
    if (have_rx && valid_lat_lon(rx_lat, rx_lon))
    {
        wr_str(&tail, ",\"rx_lat\":");
        wr_fixed(&tail, rx_lat, 6);
        wr_str(&tail, ",\"rx_lon\":");
        wr_fixed(&tail, rx_lon, 6);
    }
    wr_str(&tail, "}");

    buf[0] = '\0';
    wr_str(&msg, "{\"aircraft\":[");

    while (*next < count)
    {
        char ebuf[MQTT_BATCH_ENTRY_MAX + 1];
        wr_t e = {ebuf, sizeof(ebuf), 0, false};
        ebuf[0] = '\0';
        wr_entry(&e, &list[*next]);
        if (e.overflow)
        {
            // Cannot happen with the field bounds above; skip rather than
            // send a truncated object.
            (*next)++;
            continue;
        }
        // entry + separating comma + tail + NUL must fit
        if (msg.len + (taken ? 1 : 0) + e.len + tail.len + 1 > cap)
            break; // full: this entry starts the next message
        if (taken)
            wr_raw(&msg, ",", 1);
        wr_raw(&msg, ebuf, e.len);
        taken++;
        (*next)++;
    }

    if (taken == 0)
        return 0; // only possible if every remaining entry was skipped
    wr_raw(&msg, tail_buf, tail.len);
    return msg.overflow ? 0 : msg.len;
}
