// app_main.c - what the work around the decoder costs on the device's
// processor, before and after coh4 (cost_app_rv32.py builds and runs it
// under qemu-riscv32 and counts instructions).
//
// "old": class_driver.c as it was up to coh3 - a double-precision
// haversine() per aircraft and per message, printf "%f" for every number.
// "new": the same output through adsb_fmt.c, as class_driver.c does now.
// The functions are copies of the firmware's, reduced to what they compute.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <reent.h>
#include <sys/types.h>
#include "adsb_fmt.h"

struct _reent *__getreent(void) { return _GLOBAL_REENT; }   // the library asks the RTOS for this
// a heap for the C library's number formatting
static char heap[1 << 20];
static char *brk_at = heap;
void *_sbrk(ptrdiff_t n) { char *p = brk_at; if (brk_at + n > heap + sizeof heap) return (void *)-1; brk_at += n; return p; }

typedef struct { uint32_t icao; char callsign[9]; int altitude, speed, heading, vert_rate, squawk;
                 double lat, lon; int has_position, active; } aircraft_t;
#define AIRCRAFT_TABLE_SIZE 256
static aircraft_t aircraft_table[AIRCRAFT_TABLE_SIZE];
typedef struct { double lat, lon, hdop; int fix_valid, sats; } receiver_pos_t;
static receiver_pos_t RX = { 37.8044, -122.2712, 0.9, 1, 11 };

__attribute__((noinline)) static void haversine(double lat1, double lon1, double lat2, double lon2,
                      double *dist_km, double *bearing_deg) {
    double d2r = M_PI / 180.0;
    double rlat1 = lat1 * d2r, rlat2 = lat2 * d2r;
    double dlat = (lat2 - lat1) * d2r;
    double dlon = (lon2 - lon1) * d2r;
    double a = sin(dlat/2) * sin(dlat/2) +
               cos(rlat1) * cos(rlat2) * sin(dlon/2) * sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    *dist_km = 6371.0 * c;
    double y = sin(dlon) * cos(rlat2);
    double x = cos(rlat1) * sin(rlat2) - sin(rlat1) * cos(rlat2) * cos(dlon);
    double brg = atan2(y, x) * 180.0 / M_PI;
    if (brg < 0) brg += 360.0;
    *bearing_deg = brg;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static void fill(int n, int pos_pct) {
    memset(aircraft_table, 0, sizeof aircraft_table);
    for (int k = 0; k < n; k++) {
        int i; do i = rnd() % AIRCRAFT_TABLE_SIZE; while (aircraft_table[i].active);
        aircraft_t *a = &aircraft_table[i];
        a->active = 1; a->icao = 0xA00000 + rnd() % 0xFFFFF;
        if (rnd() % 100 < 85) snprintf(a->callsign, sizeof a->callsign, "UAL%u", (unsigned)(rnd() % 3000));
        a->altitude = 1000 + (int)(rnd() % 40000); a->speed = 120 + (int)(rnd() % 400);
        a->heading = 1 + (int)(rnd() % 359); a->squawk = 1200; a->vert_rate = -64;
        if ((int)(rnd() % 100) < pos_pct) {
            a->has_position = 1;
            a->lat = RX.lat + ((int)(rnd() % 6000) - 3000) / 1000.0;
            a->lon = RX.lon + ((int)(rnd() % 8000) - 4000) / 1000.0;
        }
    }
}

// ---- the list (adsb_format_aircraft_list), sorted by distance ----
__attribute__((noinline)) static int old_list(char *buf, int bufsize) {
    int pos = 0; receiver_pos_t rx = RX;
    typedef struct { int idx; double dist_km; } ac_entry_t;
    ac_entry_t entries[AIRCRAFT_TABLE_SIZE];
    int count = 0;
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
        if (!aircraft_table[i].active) continue;
        entries[count].idx = i; entries[count].dist_km = 1e9;
        if (rx.fix_valid && aircraft_table[i].has_position) {
            double d, b; haversine(rx.lat, rx.lon, aircraft_table[i].lat, aircraft_table[i].lon, &d, &b);
            entries[count].dist_km = d;
        }
        count++;
    }
    for (int i = 1; i < count; i++) {
        ac_entry_t key = entries[i]; int j = i - 1;
        while (j >= 0) {
            int cmp = (entries[j].dist_km > key.dist_km) - (entries[j].dist_km < key.dist_km);
            if (cmp <= 0) break;
            entries[j + 1] = entries[j]; j--;
        }
        entries[j + 1] = key;
    }
    for (int e = 0; e < count && pos < bufsize - 80; e++) {
        aircraft_t *ac = &aircraft_table[entries[e].idx];
        char call[9] = "--------";
        if (ac->callsign[0]) snprintf(call, sizeof(call), "%-8s", ac->callsign);
        double dist_nm = entries[e].dist_km * 0.539957;
        char dist_str[12] = "   --";
        if (rx.fix_valid && ac->has_position && entries[e].dist_km < 1e8) snprintf(dist_str, sizeof(dist_str), "%5.1f", dist_nm);
        char alt_str[12] = "  ----";
        if (ac->altitude != 0) snprintf(alt_str, sizeof(alt_str), "%6d", ac->altitude);
        char spd_str[12] = " ---";
        if (ac->speed > 0) snprintf(spd_str, sizeof(spd_str), "%4d", ac->speed);
        char hdg_str[12] = "---";
        if (ac->heading > 0) snprintf(hdg_str, sizeof(hdg_str), "%3d", (int)ac->heading);
        pos += snprintf(buf + pos, bufsize - pos, "%06lX %-8s%s %s %s %s\n",
            (unsigned long)ac->icao, call, alt_str, spd_str, hdg_str, dist_str);
    }
    return count;
}

typedef struct { uint32_t icao; char callsign[9]; uint8_t has_position; int altitude, speed, heading; double lat, lon; float dist_km; } list_row_t;
static list_row_t s_list_rows[AIRCRAFT_TABLE_SIZE];
// the part under the lock: a copy
__attribute__((noinline)) static int new_list_copy(void) {
    int count = 0;
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
        const aircraft_t *a = &aircraft_table[i];
        if (!a->active) continue;
        list_row_t *r = &s_list_rows[count++];
        r->icao = a->icao; memcpy(r->callsign, a->callsign, sizeof(r->callsign));
        r->has_position = a->has_position ? 1 : 0;
        r->altitude = a->altitude; r->speed = a->speed; r->heading = a->heading; r->lat = a->lat; r->lon = a->lon;
    }
    return count;
}
// the rest, outside it
__attribute__((noinline)) static int new_list_rest(int count, char *buf, int bufsize) {
    int pos = 0; receiver_pos_t rx = RX;
    for (int i = 0; i < count; i++) {
        list_row_t *r = &s_list_rows[i];
        r->callsign[8] = '\0'; r->dist_km = 1e9f;
        if (rx.fix_valid && r->has_position) { float brg; adsb_gc(rx.lat, rx.lon, r->lat, r->lon, &r->dist_km, &brg); }
    }
    uint16_t order[AIRCRAFT_TABLE_SIZE];
    for (int i = 0; i < count; i++) {
        int j = i - 1;
        while (j >= 0) {
            const list_row_t *a = &s_list_rows[order[j]], *b = &s_list_rows[i];
            int cmp = (a->dist_km > b->dist_km) - (a->dist_km < b->dist_km);
            if (cmp <= 0) break;
            order[j + 1] = order[j]; j--;
        }
        order[j + 1] = (uint16_t)i;
    }
    for (int e = 0; e < count && pos < bufsize - 80; e++) {
        const list_row_t *ac = &s_list_rows[order[e]];
        char call[9] = "--------";
        if (ac->callsign[0]) snprintf(call, sizeof(call), "%-8s", ac->callsign);
        char dist_str[12] = "   --";
        if (rx.fix_valid && ac->has_position && ac->dist_km < 1e8f) adsb_put_fixed_f(dist_str, sizeof(dist_str), ac->dist_km * 0.539957f, 1, 5, ' ');
        char alt_str[12] = "  ----";
        if (ac->altitude != 0) snprintf(alt_str, sizeof(alt_str), "%6d", ac->altitude);
        char spd_str[12] = " ---";
        if (ac->speed > 0) snprintf(spd_str, sizeof(spd_str), "%4d", ac->speed);
        char hdg_str[12] = "---";
        if (ac->heading > 0) snprintf(hdg_str, sizeof(hdg_str), "%3d", (int)ac->heading);
        pos += snprintf(buf + pos, bufsize - pos, "%06lX %-8s%s %s %s %s\n",
            (unsigned long)ac->icao, call, alt_str, spd_str, hdg_str, dist_str);
    }
    return count;
}

// ---- nearest and farthest (adsb_get_stats; the scope's snapshot was the same loop) ----
__attribute__((noinline)) static double old_pass(void) {
    double best = 1e9, worst = 0;
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
        if (!aircraft_table[i].active || !aircraft_table[i].has_position) continue;
        double d, b; haversine(RX.lat, RX.lon, aircraft_table[i].lat, aircraft_table[i].lon, &d, &b);
        if (d < best) best = d;
        if (d > worst) worst = d;
    }
    return best + worst;
}
__attribute__((noinline)) static float new_pass(void) {
    const float cos_rx = adsb_gc_cos(RX.lat);
    float best = 9.0f, worst = -1.0f; int ni = -1, fi = -1;
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
        const aircraft_t *a = &aircraft_table[i];
        if (!a->active || !a->has_position) continue;
        const float rank = adsb_gc_rank(RX.lat, RX.lon, cos_rx, a->lat, a->lon);
        if (rank < best) { best = rank; ni = i; }
        if (rank > worst) { worst = rank; fi = i; }
    }
    float km = 0, km2 = 0, brg;
    if (ni >= 0) adsb_gc(RX.lat, RX.lon, aircraft_table[ni].lat, aircraft_table[ni].lon, &km, &brg);
    if (fi >= 0) adsb_gc(RX.lat, RX.lon, aircraft_table[fi].lat, aircraft_table[fi].lon, &km2, &brg);
    return km + km2;
}

// ---- one decoded message (on_msg): console line, SD card row, MQTT distance ----
static char sd_buf[512];
__attribute__((noinline)) static int old_msg(const aircraft_t *ac, const unsigned char *msg, int msgbits, char *line, int avail) {
    int pos = 0; uint32_t icao = ac->icao;
    { int n = snprintf(line + pos, avail, "%06lX", (unsigned long)icao); if (n > 0) { pos += n; avail -= n; } }
    if (ac->callsign[0]) { int n = snprintf(line + pos, avail, " %-8s", ac->callsign); if (n > 0) { pos += n; avail -= n; } }
    if (ac->altitude) { int n = snprintf(line + pos, avail, " %5dft", ac->altitude); if (n > 0) { pos += n; avail -= n; } }
    if (ac->speed) { int n = snprintf(line + pos, avail, " %3dkt %03d°", ac->speed, ac->heading); if (n > 0) { pos += n; avail -= n; } }
    if (ac->vert_rate) { int n = snprintf(line + pos, avail, " %+dfpm", ac->vert_rate); if (n > 0) { pos += n; avail -= n; } }
    if (ac->has_position) {
        int n = snprintf(line + pos, avail, " (%.4f%c,%.4f%c)", fabs(ac->lat), ac->lat >= 0 ? 'N' : 'S', fabs(ac->lon), ac->lon >= 0 ? 'E' : 'W');
        if (n > 0) { pos += n; avail -= n; }
        receiver_pos_t rx = RX;
        if (rx.fix_valid) {
            double dist_km, brg; haversine(rx.lat, rx.lon, ac->lat, ac->lon, &dist_km, &brg);
            double dist_nm = dist_km * 0.539957;
            n = snprintf(line + pos, avail, " %.1fnm@%03.0f°", dist_nm, brg);
            if (n > 0) { pos += n; avail -= n; }
        }
    }
    if (ac->squawk) { int n = snprintf(line + pos, avail, " SQK:%04d", ac->squawk); if (n > 0) { pos += n; avail -= n; } }
    { int n = snprintf(line + pos, avail, " ["); if (n > 0) { pos += n; avail -= n; } }
    int msglen = msgbits / 8;
    for (int i = 0; i < msglen && avail > 3; i++) { int n = snprintf(line + pos, avail, "%02X", msg[i]); if (n > 0) { pos += n; avail -= n; } }
    if (avail > 2) { line[pos++] = ']'; line[pos++] = '\n'; line[pos] = '\0'; }
    // the SD row
    {
        receiver_pos_t rx = RX; double range_km = 0, bearing = 0; int p = 0, n;
        if (rx.fix_valid && ac->has_position) haversine(rx.lat, rx.lon, ac->lat, ac->lon, &range_km, &bearing);
        n = snprintf(sd_buf + p, sizeof sd_buf - p, ","); if (n > 0) p += n;
        for (int i = 0; i < msglen; i++) { n = snprintf(sd_buf + p, 3, "%02X", msg[i]); if (n > 0) p += n; }
        n = snprintf(sd_buf + p, sizeof sd_buf - p, ",%06lX,%s,%d,%d,%d,%d,%.5f,%.5f,%04d,%.5f,%.5f,%.1f,%.0f,%d,%.1f\n",
            (unsigned long)ac->icao, ac->callsign[0] ? ac->callsign : "", ac->altitude, ac->speed, ac->heading, ac->vert_rate,
            ac->lat, ac->lon, ac->squawk, rx.fix_valid ? rx.lat : 0.0, rx.fix_valid ? rx.lon : 0.0, range_km, bearing, rx.sats, rx.hdop);
        if (n > 0) p += n;
        pos += p;
    }
    // MQTT
    if (RX.fix_valid && ac->has_position) { double dist_km, brg; haversine(RX.lat, RX.lon, ac->lat, ac->lon, &dist_km, &brg); pos += (int)(float)(dist_km * 0.539957) + (int16_t)brg; }
    return pos;
}
__attribute__((noinline)) static int new_msg(const aircraft_t *ac, const unsigned char *msg, int msgbits, char *line, int avail) {
    int pos = 0; uint32_t icao = ac->icao;
    const aircraft_t snap = *ac;                       // the copy taken under the lock
    const receiver_pos_t rx = RX;
    const int have_range = rx.fix_valid && snap.has_position;
    float range_km = 0.0f, bearing = 0.0f;
    if (have_range) adsb_gc(rx.lat, rx.lon, snap.lat, snap.lon, &range_km, &bearing);
#define LINE_PUT(expr) do { int n_ = (expr); if (n_ > 0 && n_ < avail) { pos += n_; avail -= n_; } } while (0)
    LINE_PUT(snprintf(line + pos, avail, "%06lX", (unsigned long)icao));
    if (snap.callsign[0]) LINE_PUT(snprintf(line + pos, avail, " %-8s", snap.callsign));
    if (snap.altitude)  LINE_PUT(snprintf(line + pos, avail, " %5dft", snap.altitude));
    if (snap.speed)     LINE_PUT(snprintf(line + pos, avail, " %3dkt %03d°", snap.speed, snap.heading));
    if (snap.vert_rate) LINE_PUT(snprintf(line + pos, avail, " %+dfpm", snap.vert_rate));
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
    if (snap.squawk) LINE_PUT(snprintf(line + pos, avail, " SQK:%04d", snap.squawk));
    LINE_PUT(snprintf(line + pos, avail, " ["));
    int msglen = msgbits / 8;
    LINE_PUT(adsb_put_hex(line + pos, avail, msg, msglen));
#undef LINE_PUT
    if (avail > 2) { line[pos++] = ']'; line[pos++] = '\n'; line[pos] = '\0'; }
    // the SD row
    {
        char *b = sd_buf; int left = sizeof sd_buf, w = 0;
#define SD_PUT(expr) do { int k_ = (expr); if (k_ > 0 && k_ < left - w) w += k_; } while (0)
        SD_PUT(snprintf(b + w, left - w, ","));
        SD_PUT(adsb_put_hex(b + w, left - w, msg, msglen));
        SD_PUT(snprintf(b + w, left - w, ",%06lX,%s,%d,%d,%d,%d,", (unsigned long)snap.icao, snap.callsign[0] ? snap.callsign : "",
                        snap.altitude, snap.speed, snap.heading, snap.vert_rate));
        SD_PUT(adsb_put_fixed(b + w, left - w, snap.lat, 5, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ","));
        SD_PUT(adsb_put_fixed(b + w, left - w, snap.lon, 5, 0, ' '));
        SD_PUT(snprintf(b + w, left - w, ",%04d,", snap.squawk));
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
        pos += w;
    }
    if (have_range) pos += (int)(range_km * 0.539957f) + (int16_t)bearing;   // MQTT
    return pos;
}

static char out[AIRCRAFT_TABLE_SIZE * 37 + 128];
static void put(const char *s) { if (write(1, s, strlen(s)) < 0) {} }

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "none";
    int n = argc > 2 ? atoi(argv[2]) : 100;
    int reps = argc > 3 ? atoi(argv[3]) : 1;
    fill(n, 80);                       // 80 % of the aircraft have a position
    volatile double sink = 0; int r = 0, cnt = 0;
    static const unsigned char msg[14] = {0x8D,0xA1,0x23,0x45,0x58,0xC3,0x82,0xD6,0x90,0xC8,0xAC,0x28,0x63,0xA7};
    char line[384];
    const aircraft_t *with = NULL, *without = NULL;
    for (int i = 0; i < AIRCRAFT_TABLE_SIZE; i++) {
        if (aircraft_table[i].active && aircraft_table[i].has_position && !with) with = &aircraft_table[i];
        if (aircraft_table[i].active && !aircraft_table[i].has_position && !without) without = &aircraft_table[i];
    }
    for (int k = 0; k < reps; k++) {
        if (!strcmp(what, "old_list")) r += old_list(out, sizeof out);
        else if (!strcmp(what, "new_list_copy")) r += new_list_copy();
        else if (!strcmp(what, "new_list")) { cnt = new_list_copy(); r += new_list_rest(cnt, out, sizeof out); }
        else if (!strcmp(what, "old_pass")) sink += old_pass();
        else if (!strcmp(what, "new_pass")) sink += new_pass();
        else if (!strcmp(what, "old_msg")) r += old_msg(with, msg, 112, line, sizeof line);
        else if (!strcmp(what, "new_msg")) r += new_msg(with, msg, 112, line, sizeof line);
        else if (!strcmp(what, "old_msg_nopos")) r += old_msg(without, msg, 56, line, sizeof line);
        else if (!strcmp(what, "new_msg_nopos")) r += new_msg(without, msg, 56, line, sizeof line);
    }
    // "same": the two versions' text, for the caller to compare
    if (!strcmp(what, "show_old")) { old_list(out, sizeof out); put(out); old_msg(with, msg, 112, line, sizeof line); put(line); put(sd_buf); }
    if (!strcmp(what, "show_new")) { cnt = new_list_copy(); new_list_rest(cnt, out, sizeof out); put(out); new_msg(with, msg, 112, line, sizeof line); put(line); put(sd_buf); }
    return (r + (int)sink) & 0;
}
