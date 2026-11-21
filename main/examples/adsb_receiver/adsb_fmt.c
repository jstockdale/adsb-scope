/*
 * adsb_fmt.c - see adsb_fmt.h.
 */
#include "adsb_fmt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define D2R_F 0.017453292519943295f
#define R2D_F 57.29577951308232f

// lon2 - lon1 in degrees, brought into -180..180, as a float
static inline float dlon_deg(double lon1, double lon2)
{
    double d = lon2 - lon1;
    if (d > 180.0) d -= 360.0;
    else if (d < -180.0) d += 360.0;
    return (float)d;
}

void adsb_gc(double lat1, double lon1, double lat2, double lon2,
             float *dist_km, float *bearing_deg)
{
    const float dlat = (float)(lat2 - lat1) * D2R_F;
    const float dlon = dlon_deg(lon1, lon2) * D2R_F;
    const float rlat1 = (float)lat1 * D2R_F, rlat2 = (float)lat2 * D2R_F;
    const float c1 = cosf(rlat1), c2 = cosf(rlat2), s1 = sinf(rlat1);
    const float shl = sinf(dlat * 0.5f), sho = sinf(dlon * 0.5f);

    float a = shl * shl + c1 * c2 * sho * sho;
    if (a > 1.0f) a = 1.0f;
    *dist_km = 6371.0f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));

    // cos(lat1) sin(lat2) - sin(lat1) cos(lat2) cos(dlon), with
    // cos(dlon) = 1 - 2 sin^2(dlon/2): no difference of nearly equal terms
    const float y = sinf(dlon) * c2;
    const float x = sinf(dlat) + 2.0f * s1 * c2 * sho * sho;
    float b = atan2f(y, x) * R2D_F;
    if (b < 0.0f) b += 360.0f;
    *bearing_deg = b;
}

float adsb_gc_cos(double lat_deg)
{
    return cosf((float)lat_deg * D2R_F);
}

float adsb_gc_rank(double lat1, double lon1, float cos_lat1, double lat2, double lon2)
{
    const float shl = sinf((float)(lat2 - lat1) * (0.5f * D2R_F));
    const float sho = sinf(dlon_deg(lon1, lon2) * (0.5f * D2R_F));
    return shl * shl + cos_lat1 * cosf((float)lat2 * D2R_F) * sho * sho;
}

// sign, the digits of `scaled` with a point `decimals` from the right,
// padding on the left
static int put_scaled(char *buf, int avail, int negative, uint32_t scaled, int decimals, int width, char pad)
{
    char t[16];   // digits, least significant first
    int n = 0;
    do {
        t[n++] = (char)('0' + scaled % 10u);
        scaled /= 10u;
    } while (scaled != 0 || n <= decimals);   // at least one digit before the point
    const int len = n + (decimals > 0) + (negative != 0);
    const int fill = width > len ? width - len : 0;
    if (len + fill >= avail) return 0;
    char *p = buf;
    if (pad == '0') {
        if (negative) *p++ = '-';
        for (int i = 0; i < fill; i++) *p++ = '0';
    } else {
        for (int i = 0; i < fill; i++) *p++ = ' ';
        if (negative) *p++ = '-';
    }
    while (n > decimals) *p++ = t[--n];
    if (decimals > 0) {
        *p++ = '.';
        while (n > 0) *p++ = t[--n];
    }
    *p = '\0';
    return (int)(p - buf);
}

static const double POW10[7] = { 1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0 };
static const float POW10F[7] = { 1.0f, 10.0f, 100.0f, 1000.0f, 10000.0f, 100000.0f, 1000000.0f };

static int put_printf(char *buf, int avail, double v, int decimals, int width, char pad)
{
    int n = snprintf(buf, (size_t)avail, pad == '0' ? "%0*.*f" : "%*.*f", width, decimals, v);
    return (n > 0 && n < avail) ? n : 0;
}

// a*b - p exactly, where p is a*b as the processor rounded it (Dekker's
// product: no fused multiply-add needed, and the C library's fma() on this
// target is not exact). Only called when the rounded product sits on a tie.
__attribute__((optimize("fp-contract=off")))
static double product_error(double a, double b, double p)
{
    const double split = 134217729.0;   // 2^27 + 1
    double t = a * split;
    const double ah = t - (t - a), al = a - ah;
    t = b * split;
    const double bh = t - (t - b), bl = b - bh;
    return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

int adsb_put_fixed(char *buf, int avail, double v, int decimals, int width, char pad)
{
    if (avail <= 0) return 0;
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    const int negative = signbit(v) != 0;
    const double mag = negative ? -v : v;
    const double scaled = mag * POW10[decimals];
    if (!(scaled < 4.0e9)) return put_printf(buf, avail, v, decimals, width, pad);   // also NaN, infinity
    uint32_t r = (uint32_t)scaled;                 // towards zero
    const double frac = scaled - (double)r;        // exact
    if (frac > 0.5) {
        r++;
    } else if (frac == 0.5) {
        // The rounded product is on a tie; the value itself may be just
        // above or below it (0.05 is a little more than 5/100).
        const double e = product_error(mag, POW10[decimals], scaled);
        if (e > 0.0 || (e == 0.0 && (r & 1u))) r++;
    }
    return put_scaled(buf, avail, negative, r, decimals, width, pad);
}

int adsb_put_fixed_f(char *buf, int avail, float v, int decimals, int width, char pad)
{
    if (avail <= 0) return 0;
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    const int negative = signbit(v) != 0;
    const float scaled = (negative ? -v : v) * POW10F[decimals] + 0.5f;
    if (!(scaled < 4.0e9f)) return put_printf(buf, avail, (double)v, decimals, width, pad);
    return put_scaled(buf, avail, negative, (uint32_t)scaled, decimals, width, pad);
}

int adsb_put_hex(char *buf, int avail, const uint8_t *bytes, int n)
{
    static const char H[] = "0123456789ABCDEF";
    if (n < 0 || 2 * n >= avail) return 0;
    for (int i = 0; i < n; i++) {
        buf[2 * i] = H[bytes[i] >> 4];
        buf[2 * i + 1] = H[bytes[i] & 15];
    }
    buf[2 * n] = '\0';
    return 2 * n;
}
