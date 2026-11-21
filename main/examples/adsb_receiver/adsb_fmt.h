/*
 * adsb_fmt.h - distance and number formatting for the per-message path.
 *
 * The ESP32-P4 has a single-precision FPU; double arithmetic is done in
 * software. One double-precision haversine() is about 38,000 instructions,
 * and printf's "%f" is several thousand more per number, and the receiver
 * did both several times for every decoded message and for every aircraft
 * on every refresh of the list (docs/UPLINK.md section 9.7).
 *
 * What is here does the same work without double arithmetic where the
 * result allows it: distance and bearing in single precision, and decimal
 * formatting by scaling and rounding.
 *
 * Plain C, no ESP-IDF: built on a PC by harness/fmt/test_fmt.c.
 */
#ifndef ADSB_FMT_H
#define ADSB_FMT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Great-circle distance in km and initial bearing in degrees (0 to 360)
// from point 1 to point 2. The coordinate differences are taken in double
// (two latitudes 100 m apart are the same float) and everything after that
// is single precision, in forms that do not subtract nearly equal numbers:
// within 0.001 km and 0.001 degrees of the double-precision haversine at
// every range this receiver sees (harness/results/fmt.txt).
void adsb_gc(double lat1, double lon1, double lat2, double lon2,
             float *dist_km, float *bearing_deg);

// For finding the nearest or the farthest of many points without a
// distance for each: a value that grows with the distance from point 1.
// cos_lat1 is adsb_gc_cos(lat1), computed once.
float adsb_gc_cos(double lat_deg);
float adsb_gc_rank(double lat1, double lon1, float cos_lat1, double lat2, double lon2);

// Decimal text of v with `decimals` digits after the point (0 to 6), as
// printf("%*.*f") writes it: rounded to nearest, an exact tie to even, a
// minus sign for any negative value. Right-aligned in `width` with `pad`
// (' ' or '0'; width 0 for none). Appends to buf, which has `avail` bytes
// left including the terminator; returns the characters written, 0 if
// there was no room. |v| * 10^decimals must be below 4e9 (it falls back to
// snprintf above that).
int adsb_put_fixed(char *buf, int avail, double v, int decimals, int width, char pad);

// The same for a float, in single-precision arithmetic (rounds half up).
int adsb_put_fixed_f(char *buf, int avail, float v, int decimals, int width, char pad);

// n bytes as upper-case hex. Returns 2*n, or 0 if there was no room.
int adsb_put_hex(char *buf, int avail, const uint8_t *bytes, int n);

#ifdef __cplusplus
}
#endif

#endif // ADSB_FMT_H
