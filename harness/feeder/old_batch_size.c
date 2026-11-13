// harness/feeder/old_batch_size.c – how large were the feeder's messages
// before r7, and how large are they now?
//
// Builds the same aircraft entries twice: the way mqtt_feeder.c did up to
// r6 (a cJSON tree printed with cJSON_PrintUnformatted, using ESP-IDF's own
// cJSON so the number formatting is the firmware's), and with the r7
// serialiser. Positions are arbitrary doubles, as a CPR decode produces
// them, and the distance is a float, as in the firmware.
//
//   make old_batch_size IDF_PATH=/path/to/esp-idf && ./old_batch_size
#include "cJSON.h"
#include "mqtt_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state = 0x1234567887654321ull;
static double rnd_unit(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (double)((rng_state * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}

// An aircraft in the state the tracker typically holds after a minute:
// everything known. full = 0 leaves out callsign and position (a target
// heard only through short replies).
static mqtt_aircraft_t make(int i, int full)
{
    mqtt_aircraft_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.icao, sizeof(a.icao), "%06X", 0xA00000 + (unsigned)(rnd_unit() * 0xFFFFF));
    a.alt_ft = 1000 + (int)(rnd_unit() * 40000);
    if (full)
    {
        snprintf(a.callsign, sizeof(a.callsign), "UAL%d", 100 + i);
        a.lat = 37.0 + rnd_unit() * 2.0;
        a.lon = -123.0 + rnd_unit() * 2.0;
        a.has_pos = true;
        a.speed_kt = (int16_t)(120 + rnd_unit() * 400);
        a.heading_deg = (int16_t)(rnd_unit() * 360);
        a.vert_rate_fpm = (int16_t)(((int)(rnd_unit() * 60) - 30) * 64);
        a.dist_nm = (float)(rnd_unit() * 200.0);
        a.bearing_deg = (int16_t)(rnd_unit() * 360);
    }
    return a;
}

// mqtt_feeder.c pub_batch() as it was up to r6
static size_t old_size(const mqtt_aircraft_t *batch, int count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "aircraft");
    char *json;
    size_t len;
    int i;
    cJSON_AddNumberToObject(root, "rx_lat", 37.61904312);
    cJSON_AddNumberToObject(root, "rx_lon", -122.37483921);
    for (i = 0; i < count; i++)
    {
        const mqtt_aircraft_t *ac = &batch[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "icao", ac->icao);
        if (ac->callsign[0])        cJSON_AddStringToObject(obj, "callsign", ac->callsign);
        if (ac->has_pos)
        {
            cJSON_AddNumberToObject(obj, "lat", ac->lat);
            cJSON_AddNumberToObject(obj, "lon", ac->lon);
        }
        if (ac->alt_ft != 0)        cJSON_AddNumberToObject(obj, "alt", ac->alt_ft);
        if (ac->speed_kt != 0)      cJSON_AddNumberToObject(obj, "speed", ac->speed_kt);
        if (ac->heading_deg >= 0)   cJSON_AddNumberToObject(obj, "hdg", ac->heading_deg);
        if (ac->vert_rate_fpm != 0) cJSON_AddNumberToObject(obj, "vr", ac->vert_rate_fpm);
        if (ac->dist_nm > 0)        cJSON_AddNumberToObject(obj, "dist", ac->dist_nm);
        if (ac->bearing_deg >= 0)   cJSON_AddNumberToObject(obj, "brg", ac->bearing_deg);
        cJSON_AddItemToArray(arr, obj);
    }
    json = cJSON_PrintUnformatted(root);
    len = strlen(json);
    if (count == 1)
        printf("  sample entry, old: %s\n", json);
    free(json);
    cJSON_Delete(root);
    return len;
}

static size_t new_size(const mqtt_aircraft_t *batch, int count, int *messages)
{
    static char buf[3800];
    int next = 0;
    size_t total = 0, len;
    *messages = 0;
    while ((len = mqtt_batch_serialise(batch, count, &next, true, 37.61904312, -122.37483921, buf, sizeof(buf))) > 0)
    {
        if (count == 1)
            printf("  sample entry, new: %s\n", buf);
        total += len;
        (*messages)++;
    }
    return total;
}

int main(void)
{
    static mqtt_aircraft_t list[MQTT_BATCH_SLOTS];
    static const int counts[] = {1, 20, 64, 100, 256};
    int i, messages;
    size_t c;

    for (i = 0; i < MQTT_BATCH_SLOTS; i++)
        list[i] = make(i, 1);
    printf("All fields known (position, velocity, callsign, distance):\n");
    printf("  %8s %12s %12s %10s %12s\n", "entries", "old bytes", "new bytes", "new msgs", "new/entry");
    for (c = 0; c < sizeof(counts) / sizeof(counts[0]); c++)
    {
        size_t o = old_size(list, counts[c]);
        size_t n = new_size(list, counts[c], &messages);
        printf("  %8d %12zu %12zu %10d %12.1f\n", counts[c], o, n, messages, (double)n / counts[c]);
    }
    for (i = 0; i < MQTT_BATCH_SLOTS; i++)
        list[i] = make(i, 0);
    printf("Address and altitude only:\n");
    for (c = 0; c < sizeof(counts) / sizeof(counts[0]); c++)
    {
        size_t o = old_size(list, counts[c]);
        size_t n = new_size(list, counts[c], &messages);
        printf("  %8d %12zu %12zu %10d %12.1f\n", counts[c], o, n, messages, (double)n / counts[c]);
    }
    printf("The old feeder sent at most 64 entries per batch, in one MQTT message.\n");
    return 0;
}
