// harness/feeder/test_batch.c – host tests for the MQTT batch module
// (main/examples/adsb_receiver/mqtt_batch.c).
//
// Checks the coalescing logic directly, and writes every JSON message the
// serialiser produces – for fixed, adversarial and random inputs – to
// stdout in a line format that check_feeder.py validates with Python's own
// JSON parser:
//
//   CASE <id> <aircraft> <cap> <have_rx>
//   E <id> <icao bytes as hex, or - if empty> <1 if a position must be present>
//   M <id> <length> <json>
//   END <id>
//
// Exit status is the number of failed C-level checks.
#include "mqtt_batch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);    \
            fails++;                                                \
        }                                                           \
    } while (0)

// xorshift64*: deterministic, no library dependence
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}
static double rnd_unit(void) { return (double)(rnd() >> 11) / 9007199254740992.0; }

static mqtt_aircraft_t plain(unsigned icao, int v)
{
    mqtt_aircraft_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.icao, sizeof(a.icao), "%06X", icao & 0xFFFFFF);
    snprintf(a.callsign, sizeof(a.callsign), "TST%04d", v % 10000);
    a.lat = 37.0 + (v % 100) * 0.01;
    a.lon = -122.0 - (v % 100) * 0.01;
    a.alt_ft = 1000 + v;
    a.speed_kt = 300;
    a.heading_deg = 90;
    a.vert_rate_fpm = -640;
    a.dist_nm = 10.25f;
    a.bearing_deg = 45;
    a.has_pos = true;
    return a;
}

static bool pos_expected(const mqtt_aircraft_t *a)
{
    return a->has_pos && isfinite(a->lat) && isfinite(a->lon) &&
           a->lat >= -90.0 && a->lat <= 90.0 && a->lon >= -180.0 && a->lon <= 180.0;
}

static int case_id;

// Serialise a list completely at the given buffer size, checking the
// invariants the feeder relies on and printing everything for the Python
// side. Returns the number of messages.
static int run_case(const mqtt_aircraft_t *list, int count, size_t cap, bool have_rx,
                    double rx_lat, double rx_lon)
{
    char *buf = malloc(cap + 16);
    int next = 0, messages = 0, i;
    int id = ++case_id;
    memset(buf, 0x55, cap + 16);

    printf("CASE %d %d %zu %d\n", id, count, cap, have_rx ? 1 : 0);
    for (i = 0; i < count; i++)
    {
        size_t k;
        printf("E %d ", id);
        if (!list[i].icao[0])
            printf("-");
        for (k = 0; k < 8 && list[i].icao[k]; k++)
            printf("%02x", (unsigned char)list[i].icao[k]);
        printf(" %d\n", pos_expected(&list[i]) ? 1 : 0);
    }
    while (next < count)
    {
        int before = next;
        size_t len = mqtt_batch_serialise(list, count, &next, have_rx, rx_lat, rx_lon, buf, cap);
        if (len == 0)
            break;
        CHECK(next > before);             // progress on every message
        CHECK(len < cap);                 // text plus NUL fits
        CHECK(buf[len] == '\0');
        CHECK(strlen(buf) == len);
        CHECK(buf[cap] == 0x55);          // nothing written past the buffer
        printf("M %d %zu %s\n", id, len, buf);
        messages++;
        if (messages > count + 1)
        {
            CHECK(!"serialiser does not terminate");
            break;
        }
    }
    CHECK(next == count);                 // every aircraft consumed
    printf("END %d\n", id);
    free(buf);
    return messages;
}

static void test_coalescing(void)
{
    static mqtt_batch_t b;
    static mqtt_aircraft_t out[MQTT_BATCH_SLOTS];
    int round, i, n;

    mqtt_batch_init(&b);
    // 100 aircraft, 5 updates each: one slot per aircraft, latest state kept
    for (round = 0; round < 5; round++)
        for (i = 0; i < 100; i++)
        {
            mqtt_aircraft_t a = plain(0xA00000 + i, round * 1000 + i);
            CHECK(mqtt_batch_put(&b, &a));
        }
    CHECK(b.count == 100);
    CHECK(b.updates == 500);
    CHECK(b.coalesced == 400);
    CHECK(b.dropped_full == 0);
    for (i = 0; i < 100; i++)
        CHECK(b.slot[i].alt_ft == 1000 + 4 * 1000 + i);

    // take: copies in arrival order, empties the slots, keeps the counters
    n = mqtt_batch_take(&b, out, MQTT_BATCH_SLOTS);
    CHECK(n == 100);
    CHECK(b.count == 0);
    CHECK(b.updates == 500);
    CHECK(strcmp(out[0].icao, "A00000") == 0 && strcmp(out[99].icao, "A00063") == 0);
    CHECK(mqtt_batch_take(&b, out, MQTT_BATCH_SLOTS) == 0);

    // after a take the same aircraft is new again
    {
        mqtt_aircraft_t a = plain(0xA00000, 1);
        CHECK(mqtt_batch_put(&b, &a));
        CHECK(b.count == 1);
        CHECK(b.coalesced == 400);
    }

    // more aircraft than slots: the first MQTT_BATCH_SLOTS are kept, the
    // rest are counted, and an aircraft already holding a slot still updates
    mqtt_batch_init(&b);
    for (i = 0; i < 300; i++)
    {
        mqtt_aircraft_t a = plain(0xB00000 + i, i);
        bool ok = mqtt_batch_put(&b, &a);
        CHECK(ok == (i < MQTT_BATCH_SLOTS));
    }
    CHECK(b.count == MQTT_BATCH_SLOTS);
    CHECK(b.dropped_full == 300 - MQTT_BATCH_SLOTS);
    {
        mqtt_aircraft_t a = plain(0xB00005, 7777);
        CHECK(mqtt_batch_put(&b, &a));
        CHECK(b.slot[5].alt_ft == 1000 + 7777);
    }
    CHECK(mqtt_batch_take(&b, out, 10) == 10);   // a short destination truncates
    CHECK(b.count == 0);

    // the key is the ICAO text up to its NUL: bytes after it do not matter,
    // and different texts never share a slot
    mqtt_batch_init(&b);
    {
        mqtt_aircraft_t a = plain(0xABCDEF, 1), c = plain(0xABCDEF, 2), d, e, f;
        a.icao[7] = 'x';              // junk after the terminator
        c.icao[7] = 'y';
        c.icao[8] = 'z';
        CHECK(mqtt_batch_put(&b, &a) && mqtt_batch_put(&b, &c));
        CHECK(b.count == 1 && b.slot[0].alt_ft == 1002);
        d = plain(0, 3); strcpy(d.icao, "ABC");
        e = plain(0, 4); strcpy(e.icao, "ABCD");
        f = plain(0, 5); f.icao[0] = '\0';
        CHECK(mqtt_batch_put(&b, &d) && mqtt_batch_put(&b, &e) && mqtt_batch_put(&b, &f));
        CHECK(b.count == 4);
        CHECK(mqtt_batch_put(&b, &f) && b.count == 4);   // empty text coalesces with itself
    }
}

static void test_golden(void)
{
    // One fully populated aircraft: the exact text, so any change to the
    // wire format shows up here.
    mqtt_aircraft_t a;
    char buf[512];
    int next = 0;
    size_t len;
    memset(&a, 0, sizeof(a));
    strcpy(a.icao, "A1B2C3");
    strcpy(a.callsign, "UAL1234");
    a.lat = 37.6213;
    a.lon = -122.379;
    a.alt_ft = 35000;
    a.speed_kt = 450;
    a.heading_deg = 270;
    a.vert_rate_fpm = -1216;
    a.dist_nm = 12.25f;
    a.bearing_deg = 93;
    a.has_pos = true;
    len = mqtt_batch_serialise(&a, 1, &next, true, 37.5, -122.25, buf, sizeof(buf));
    CHECK(strcmp(buf,
                 "{\"aircraft\":[{\"icao\":\"A1B2C3\",\"callsign\":\"UAL1234\","
                 "\"lat\":37.621300,\"lon\":-122.379000,\"alt\":35000,\"speed\":450,"
                 "\"hdg\":270,\"vr\":-1216,\"dist\":12.25,\"brg\":93}],"
                 "\"rx_lat\":37.500000,\"rx_lon\":-122.250000}") == 0);
    CHECK(len == strlen(buf) && next == 1);

    // Presence rules: nothing known but the address. hdg and brg are sent
    // whenever they are not negative, as the feeder has always done.
    memset(&a, 0, sizeof(a));
    strcpy(a.icao, "000001");
    next = 0;
    mqtt_batch_serialise(&a, 1, &next, false, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "{\"aircraft\":[{\"icao\":\"000001\",\"hdg\":0,\"brg\":0}]}") == 0);
    a.heading_deg = -1;
    a.bearing_deg = -1;
    next = 0;
    mqtt_batch_serialise(&a, 1, &next, false, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "{\"aircraft\":[{\"icao\":\"000001\"}]}") == 0);

    // Nothing to do, and a buffer too small to be useful
    next = 0;
    CHECK(mqtt_batch_serialise(&a, 0, &next, false, 0, 0, buf, sizeof(buf)) == 0);
    next = 1;
    CHECK(mqtt_batch_serialise(&a, 1, &next, false, 0, 0, buf, sizeof(buf)) == 0);
    next = 0;
    CHECK(mqtt_batch_serialise(&a, 1, &next, false, 0, 0, buf, MQTT_BATCH_MSG_MIN - 1) == 0);
    CHECK(next == 0);
}

static void test_worst_case(void)
{
    // The longest entry the format can produce: every field at its widest
    // and every string character needing a six-character escape.
    mqtt_aircraft_t a;
    char buf[MQTT_BATCH_MSG_MIN];
    int next = 0;
    size_t len;
    memset(&a, 0, sizeof(a));
    memset(a.icao, 0x01, 8);
    memset(a.callsign, 0x01, 8);
    a.lat = -90.0;
    a.lon = -180.0;
    a.alt_ft = INT32_MIN;
    a.speed_kt = INT16_MIN;
    a.heading_deg = INT16_MAX;
    a.vert_rate_fpm = INT16_MIN;
    a.dist_nm = 99999.99f;
    a.bearing_deg = INT16_MAX;
    a.has_pos = true;
    len = mqtt_batch_serialise(&a, 1, &next, true, -90.0, -180.0, buf, sizeof(buf));
    CHECK(next == 1 && len > 0);
    // envelope: {"aircraft":[ (13) + ] + ,"rx_lat":-90.000000 (20)
    //           + ,"rx_lon":-180.000000 (21) + } = 56
    fprintf(stderr, "worst-case entry: %zu characters (room for %d)\n", len - 56, MQTT_BATCH_ENTRY_MAX);
    CHECK(len - 56 == 241);
    CHECK(len - 56 <= MQTT_BATCH_ENTRY_MAX);
    CHECK(len < MQTT_BATCH_MSG_MIN);     // and it fits the smallest allowed buffer
    run_case(&a, 1, MQTT_BATCH_MSG_MIN, true, -90.0, -180.0);
}

static void test_serialise_cases(void)
{
    static mqtt_aircraft_t list[MQTT_BATCH_SLOTS];
    static const size_t caps[] = {MQTT_BATCH_MSG_MIN, 512, 1024, 3800, 4096, 65536};
    static const double specials[] = {0.0, -0.0, 90.0, -90.0, 180.0, -180.0, 90.0000001, -180.0000001,
                                      1e308, -1e308, 1e-300, 4.9e-324, NAN, INFINITY, -INFINITY, 1e15, 123456789.123456789};
    size_t c;
    int i, n, round;

    // realistic: every slot in use, all buffer sizes
    for (i = 0; i < MQTT_BATCH_SLOTS; i++)
        list[i] = plain(0xC00000 + i, i * 37);
    for (c = 0; c < sizeof(caps) / sizeof(caps[0]); c++)
    {
        n = run_case(list, MQTT_BATCH_SLOTS, caps[c], true, 37.5, -122.25);
        CHECK(n >= 1);
    }
    // the feeder's own size: 256 typical aircraft must not need an absurd
    // number of messages (about 140 characters each, 3800 per message)
    n = run_case(list, MQTT_BATCH_SLOTS, 3800, true, 37.5, -122.25);
    fprintf(stderr, "256 typical aircraft at 3800 bytes per message: %d messages\n", n);
    CHECK(n <= 12);

    // receiver position rules
    run_case(list, 3, 1024, false, 37.5, -122.25);
    run_case(list, 3, 1024, true, NAN, -122.25);     // invalid: omitted
    run_case(list, 3, 1024, true, 91.0, 0.0);        // out of range: omitted

    // adversarial strings and numbers
    n = 0;
    {
        mqtt_aircraft_t a = plain(1, 1);
        strcpy(a.callsign, "A\"B\\C");               list[n++] = a;
        memcpy(a.callsign, "\x01\x1f\x7f\x80\xff\n\t\r", 8); a.callsign[8] = 0; list[n++] = a;
        memcpy(a.icao, "\"\\\"\\\"\\\"\\", 8); a.icao[8] = 0; list[n++] = a;
        a = plain(2, 2); a.icao[0] = '\0';           list[n++] = a;
        a = plain(3, 3); a.has_pos = false; a.lat = NAN; list[n++] = a;
        a = plain(4, 4); a.dist_nm = NAN;            list[n++] = a;
        a = plain(5, 5); a.dist_nm = INFINITY;       list[n++] = a;
        a = plain(6, 6); a.dist_nm = 1e30f;          list[n++] = a;
        a = plain(7, 7); a.dist_nm = -3.0f;          list[n++] = a;
        a = plain(8, 8); a.dist_nm = 0.004f;         list[n++] = a;   // rounds to 0.00
        for (i = 0; i < (int)(sizeof(specials) / sizeof(specials[0])); i++)
        {
            a = plain(0x100 + i, i); a.lat = specials[i];             list[n++] = a;
            a = plain(0x200 + i, i); a.lon = specials[i];             list[n++] = a;
            a = plain(0x300 + i, i); a.lat = a.lon = specials[i];     list[n++] = a;
        }
    }
    for (c = 0; c < sizeof(caps) / sizeof(caps[0]); c++)
        run_case(list, n, caps[c], true, 37.5, -122.25);

    // random: any bytes, any numbers, any count, any buffer size
    for (round = 0; round < 400; round++)
    {
        int count = 1 + (int)(rnd() % MQTT_BATCH_SLOTS);
        size_t cap = (round % 3 == 0) ? caps[rnd() % 6] : MQTT_BATCH_MSG_MIN + (size_t)(rnd() % 6000);
        for (i = 0; i < count; i++)
        {
            mqtt_aircraft_t *a = &list[i];
            size_t k;
            uint64_t bits;
            unsigned char *raw = (unsigned char *)a;
            for (k = 0; k < sizeof(*a); k++)
                raw[k] = (unsigned char)rnd();
            a->icao[8] = a->callsign[8] = '\0';
            a->has_pos = (rnd() & 3) != 0;
            switch (rnd() % 4)
            {
            case 0: // arbitrary bit patterns, NaNs and infinities included
                bits = rnd(); memcpy(&a->lat, &bits, 8);
                bits = rnd(); memcpy(&a->lon, &bits, 8);
                break;
            case 1:
                a->lat = specials[rnd() % 17];
                a->lon = specials[rnd() % 17];
                break;
            default: // valid coordinates
                a->lat = rnd_unit() * 180.0 - 90.0;
                a->lon = rnd_unit() * 360.0 - 180.0;
                break;
            }
            if (rnd() & 1)
                a->dist_nm = (float)(rnd_unit() * 400.0);
        }
        run_case(list, count, cap, (rnd() & 1) != 0, rnd_unit() * 180.0 - 90.0, rnd_unit() * 360.0 - 180.0);
    }
}

int main(void)
{
    test_coalescing();
    test_golden();
    test_worst_case();
    test_serialise_cases();
    fprintf(stderr, "test_batch: %d cases written, %d C-level failures\n", case_id, fails);
    return fails > 255 ? 255 : fails;
}
