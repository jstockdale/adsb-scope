// test_fmt.c - adsb_fmt.c against the double-precision code it replaces:
// number text against printf, distance and bearing against the firmware's
// old haversine(), nearest/farthest ranking against true distance.
//   test_fmt            run the checks, exit 1 on a failure
//   test_fmt --report   also print the error figures (harness/results/fmt.txt)
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adsb_fmt.h"

// class_driver.c's haversine() as it was up to coh3
static void haversine(double lat1, double lon1, double lat2, double lon2,
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

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd64(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double rnd01(void) { return (double)(rnd64() >> 11) / 9007199254740992.0; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { if (fails < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } fails++; } } while (0)

static void one_fixed(double v, int dec, int width, char pad)
{
    char a[64], b[64];
    int n = adsb_put_fixed(a, sizeof a, v, dec, width, pad);
    int m = snprintf(b, sizeof b, pad == '0' ? "%0*.*f" : "%*.*f", width, dec, v);
    CHECK(n == m && strcmp(a, b) == 0, "put_fixed(%.17g, %d, %d, '%c') = \"%s\", printf \"%s\"", v, dec, width, pad, a, b);
}

int main(int argc, char **argv)
{
    int report = argc > 1 && strcmp(argv[1], "--report") == 0;

    // 1. Decimal text against printf
    long n_fixed = 0;
    for (long i = 0; i < 3000000; i++) {
        int dec = (int)(rnd64() % 7);
        double v;
        switch (rnd64() % 5) {
        case 0: v = (rnd01() - 0.5) * 180.0; break;                // a latitude
        case 1: v = (rnd01() - 0.5) * 360.0; break;                // a longitude
        case 2: v = rnd01() * 2000.0; break;                       // a distance, km
        case 3: v = (rnd01() - 0.5) * 2e-4; break;                 // near zero, both signs
        default: v = (double)((int64_t)(rnd64() % 4000001) - 2000000) / (double)(1u << (rnd64() % 12)); break; // dyadic: exact ties
        }
        one_fixed(v, dec, 0, ' ');
        n_fixed++;
    }
    // CPR latitudes: 6*j + 3*n/65536 and 360/59*(j + n/131072), at the SD log's and the console's digits
    for (long n = 0; n < 131072; n++) {
        double even = 36.0 + 3.0 * (double)n / 65536.0, odd = 360.0 / 59.0 * (6.0 + (double)n / 131072.0);
        one_fixed(even, 5, 0, ' '); one_fixed(-even, 5, 0, ' '); one_fixed(even, 4, 0, ' ');
        one_fixed(odd, 5, 0, ' ');  one_fixed(-odd, 5, 0, ' ');  one_fixed(odd, 4, 0, ' ');
        n_fixed += 6;
    }
    static const double edge[] = { 0.0, -0.0, 0.5, 1.5, 2.5, -0.5, -2.5, 0.125, 0.375, 9.5, 99.5, 999.5, 0.05, 0.25, 0.35,
                                   -0.000001, 0.0000004, 359.5, 359.4999, 180.0, -180.0, 89.999995, 4.2e9, -4.2e9, 1e15 };
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++)
        for (int dec = 0; dec <= 6; dec++) {
            one_fixed(edge[i], dec, 0, ' '); one_fixed(edge[i], dec, 9, ' '); one_fixed(edge[i], dec, 9, '0');
            one_fixed(edge[i], dec, 3, '0'); one_fixed(edge[i], dec, 5, ' ');
            n_fixed += 5;
        }
    {   // no room: nothing written past the end
        char s[8] = "xxxxxxx";
        CHECK(adsb_put_fixed(s, 4, 123.456, 2, 0, ' ') == 0, "put_fixed wrote into too small a buffer");
        CHECK(adsb_put_fixed(s, 7, 123.456, 2, 0, ' ') == 6 && strcmp(s, "123.46") == 0, "put_fixed exact fit: \"%s\"", s);
    }

    // 2. The float version: within one unit in the last place written of printf on the same float
    long n_ff = 0, ff_differ = 0;
    for (long i = 0; i < 1000000; i++) {
        int dec = (int)(rnd64() % 2);                 // the firmware uses 0 and 1
        float v = (float)(rnd01() * (i & 1 ? 700.0 : 360.0));
        char a[32], b[32];
        adsb_put_fixed_f(a, sizeof a, v, dec, 0, ' ');
        snprintf(b, sizeof b, "%.*f", dec, (double)v);
        if (strcmp(a, b)) { ff_differ++; CHECK(fabs(atof(a) - atof(b)) < 1.5 * pow(10, -dec), "put_fixed_f(%.9g, %d) = %s, printf %s", (double)v, dec, a, b); }
        n_ff++;
    }
    {
        char s[16];
        adsb_put_fixed_f(s, sizeof s, 12.34f, 1, 5, ' ');  CHECK(strcmp(s, " 12.3") == 0, "put_fixed_f width: \"%s\"", s);
        adsb_put_fixed_f(s, sizeof s, 7.6f, 0, 3, '0');    CHECK(strcmp(s, "008") == 0, "put_fixed_f zero pad: \"%s\"", s);
        adsb_put_fixed_f(s, sizeof s, 359.7f, 0, 3, '0');  CHECK(strcmp(s, "360") == 0, "put_fixed_f 359.7: \"%s\"", s);
        const uint8_t m[3] = { 0x8D, 0x0A, 0xF7 };
        CHECK(adsb_put_hex(s, sizeof s, m, 3) == 6 && strcmp(s, "8D0AF7") == 0, "put_hex: \"%s\"", s);
        CHECK(adsb_put_hex(s, 6, m, 3) == 0, "put_hex wrote into too small a buffer");
    }

    // 3. Distance and bearing against the double-precision haversine
    double max_dd = 0, max_db = 0, max_dd_rel = 0; long n_gc = 0, tenth_km = 0, tenth_nm = 0, whole_deg = 0;
    for (long i = 0; i < 2000000; i++) {
        double lat1 = (rnd01() - 0.5) * 150.0, lon1 = (rnd01() - 0.5) * 360.0;
        double r_nm = (i % 4 == 0) ? rnd01() * 3.0 : rnd01() * 450.0;      // a quarter close in
        double th = rnd01() * 2.0 * M_PI;
        double lat2 = lat1 + r_nm / 60.0 * cos(th);
        double lon2 = lon1 + r_nm / 60.0 * sin(th) / cos(lat1 * M_PI / 180.0);
        if (lon2 > 180.0) lon2 -= 360.0;                                   // across the antimeridian
        if (lon2 < -180.0) lon2 += 360.0;
        double d, b; float fd, fb;
        haversine(lat1, lon1, lat2, lon2, &d, &b);
        adsb_gc(lat1, lon1, lat2, lon2, &fd, &fb);
        double dd = fabs((double)fd - d), db = fabs((double)fb - b);
        if (db > 180.0) db = 360.0 - db;
        if (dd > max_dd) max_dd = dd;
        if (d > 1.0 && dd / d > max_dd_rel) max_dd_rel = dd / d;
        if (d > 0.2 && db > max_db) max_db = db;                           // bearing means little at 100 m
        char s1[24], s2[24];
        snprintf(s1, sizeof s1, "%.1f", d); adsb_put_fixed_f(s2, sizeof s2, fd, 1, 0, ' ');
        tenth_km += strcmp(s1, s2) != 0;
        snprintf(s1, sizeof s1, "%.1f", d * 0.539957); adsb_put_fixed_f(s2, sizeof s2, fd * 0.539957f, 1, 0, ' ');
        tenth_nm += strcmp(s1, s2) != 0;
        snprintf(s1, sizeof s1, "%.0f", b); adsb_put_fixed_f(s2, sizeof s2, fb, 0, 0, ' ');
        whole_deg += strcmp(s1, s2) != 0;
        n_gc++;
    }
    CHECK(max_dd < 0.001, "distance differs from the double haversine by up to %.5f km", max_dd);
    CHECK(max_db < 0.001, "bearing differs from the double haversine by up to %.5f degrees", max_db);

    // 4. Nearest and farthest by rank against true distance
    double max_pick = 0; long n_sets = 0;
    for (long s = 0; s < 20000; s++) {
        double lat1 = (rnd01() - 0.5) * 140.0, lon1 = (rnd01() - 0.5) * 360.0;
        float c1 = adsb_gc_cos(lat1), rmin = 9, rmax = -1;
        double dmin = 1e9, dmax = -1, pick_min = 0, pick_max = 0;
        for (int k = 0; k < 60; k++) {
            double lat2 = lat1 + (rnd01() - 0.5) * 8.0, lon2 = lon1 + (rnd01() - 0.5) * 10.0, d, b;
            haversine(lat1, lon1, lat2, lon2, &d, &b);
            float r = adsb_gc_rank(lat1, lon1, c1, lat2, lon2);
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
            if (r < rmin) { rmin = r; pick_min = d; }
            if (r > rmax) { rmax = r; pick_max = d; }
        }
        if (pick_min - dmin > max_pick) max_pick = pick_min - dmin;
        if (dmax - pick_max > max_pick) max_pick = dmax - pick_max;
        n_sets++;
    }
    CHECK(max_pick < 0.005, "nearest/farthest by rank is up to %.5f km from the true one", max_pick);

    if (report) {
        printf("decimal text against printf:        %ld values, %d differ\n", n_fixed, fails);
        printf("float text against printf(float):   %ld values, %ld differ, none by more than one in the last digit\n", n_ff, ff_differ);
        printf("distance against double haversine:  %ld pairs out to 450 nm, largest difference %.5f km (%.1e of the distance beyond 1 km)\n", n_gc, max_dd, max_dd_rel);
        printf("bearing against double haversine:   largest difference %.5f degrees (pairs more than 200 m apart)\n", max_db);
        printf("printed values that changed:        km to 0.1: %.3f %%   nm to 0.1: %.3f %%   bearing to 1 degree: %.3f %%\n",
               100.0 * tenth_km / n_gc, 100.0 * tenth_nm / n_gc, 100.0 * whole_deg / n_gc);
        printf("nearest/farthest by rank:           %ld sets of 60, the pick is within %.5f km of the true one\n", n_sets, max_pick);
    }
    if (fails) { printf("test_fmt: %d FAILED\n", fails); return 1; }
    printf("test_fmt: ok (%ld decimal texts, %ld distances, %ld rankings)\n", n_fixed + n_ff, n_gc, n_sets);
    return 0;
}
