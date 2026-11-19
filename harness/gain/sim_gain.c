// sim_gain.c - the firmware's gain loop (gain_ctl.c) against a model receiver.
//
// The model (madrid_model.txt, made by experiments/gain_sweep.py) holds what
// the firmware's decoder counted, in 2.5 s windows, on a recorded band put
// through an 8-bit ADC at every dB of gain from -30 to +33. A "site" says
// which of those gains the tuner's highest step corresponds to; every lower
// step is that many dB further down. Time then runs in 2.5 s slices: each
// takes one of the model's windows for the gain in use at random, thins its
// counts by the traffic of the hour, and feeds the loop exactly as
// class_driver.c does - windows of 10 s (or 200 frames after 2 s) in phase 1
// and 60 s in phase 2, one slice thrown away after every gain change.
//
// It reports where the loop spent its time and what it yielded against the
// best any FIXED gain would have yielded on the same traffic, and can be
// told what to expect (make check does).
//
// This is a model of the ADC and of the decoder, not of a tuner: in it the
// noise follows the gain dB for dB, where a real tuner's noise figure also
// changes with the setting. The device's `gain sweep` measures the real one.
//
// usage: sim_gain MODEL [--site DB] [--hours H] [--traffic F|day|night]
//                 [--start max|floor] [--seed N] [--log]
//                 [--min-yield X] [--final LO HI] [--max-changes N]
//   --site DB       model gain at the tuner's highest step (default 6)
//   --traffic F     share of the model's traffic present, 0..1 (default 1);
//                   day: 0.15..1 over 24 h; night: as day, none at all from
//                   hour 1 to hour 5
//   --min-yield X   fail unless positions decoded >= X of the best fixed gain
//   --final LO HI   fail unless the last gain is within LO..HI dB
//   --max-changes N fail if the gain changed more often than N times

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gain_ctl.h"

static const uint16_t GAINS[] = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254,
                                 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
#define NGAIN ((int)(sizeof(GAINS) / sizeof(GAINS[0])))
#define FLOOR_IDX 11
#define SLICE_S 2.5

#define DB_LO (-30)
#define DB_HI 33
#define MAXWIN 16
typedef struct
{
    double secs;
    unsigned ok, es_ok, es_lost, loud, pos, sig, delta;
    double noise, clip;
} win_t;
static win_t model[DB_HI - DB_LO + 1][MAXWIN];
static int model_n[DB_HI - DB_LO + 1];

static uint64_t rng_s = 88172645463325252ULL;
static double rnd(void)
{
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (double)(rng_s >> 11) / 9007199254740992.0;
}
static unsigned thin(unsigned n, double p)
{
    unsigned k = 0, i;
    if (p >= 1.0)
        return n;
    if (p <= 0.0)
        return 0;
    for (i = 0; i < n; i++)
        k += rnd() < p;
    return k;
}

static int site_db = 6;
static int model_db(int idx)
{
    double d = site_db - (GAINS[NGAIN - 1] - GAINS[idx]) / 10.0;
    return (int)floor(d + 0.5);
}

// Traffic present at time t (hours), 0..1
static int traffic_mode = 0; // 0 flat, 1 day, 2 night
static double traffic_flat = 1.0;
static double traffic(double t_h)
{
    double f;
    if (traffic_mode == 0)
        return traffic_flat;
    f = 0.575 - 0.425 * cos(2.0 * 3.14159265358979 * t_h / 24.0 + 1.0); // 0.15 .. 1
    if (traffic_mode == 2)
    {
        double h = fmod(t_h, 24.0);
        if (h >= 1.0 && h < 5.0)
            f = 0.0;
    }
    return f;
}

// Mean positions per second the model gives at a gain step, at full traffic
static double mean_pos(int idx)
{
    int d = model_db(idx), i;
    double pos = 0, secs = 0;
    if (d < DB_LO)
        return 0;
    if (d > DB_HI)
        d = DB_HI;
    for (i = 0; i < model_n[d - DB_LO]; i++)
    {
        pos += model[d - DB_LO][i].pos;
        secs += model[d - DB_LO][i].secs;
    }
    return secs > 0 ? pos / secs : 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    double hours = 6, min_yield = -1, final_lo = -1, final_hi = 1e9;
    int start_floor = 0, show_log = 0, max_changes = -1, i;
    FILE *f;
    char line[512];

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (a[0] != '-')
        {
            path = a;
            continue;
        }
        if (!strcmp(a, "--log"))
        {
            show_log = 1;
            continue;
        }
        if (!v)
        {
            fprintf(stderr, "error: %s needs a value\n", a);
            return 2;
        }
        i++;
        if (!strcmp(a, "--site"))
            site_db = atoi(v);
        else if (!strcmp(a, "--hours"))
            hours = atof(v);
        else if (!strcmp(a, "--seed"))
            rng_s ^= (uint64_t)strtoull(v, NULL, 0) * 0x9E3779B97F4A7C15ULL;
        else if (!strcmp(a, "--start"))
            start_floor = !strcmp(v, "floor");
        else if (!strcmp(a, "--traffic"))
        {
            if (!strcmp(v, "day"))
                traffic_mode = 1;
            else if (!strcmp(v, "night"))
                traffic_mode = 2;
            else
                traffic_flat = atof(v);
        }
        else if (!strcmp(a, "--min-yield"))
            min_yield = atof(v);
        else if (!strcmp(a, "--max-changes"))
            max_changes = atoi(v);
        else if (!strcmp(a, "--final"))
        {
            final_lo = atof(v);
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --final needs LO HI\n");
                return 2;
            }
            final_hi = atof(argv[++i]);
        }
        else
        {
            fprintf(stderr, "error: unknown option %s\n", a);
            return 2;
        }
    }
    if (!path)
    {
        fprintf(stderr, "usage: sim_gain MODEL [--site DB] [--hours H] [--traffic F|day|night] [--start max|floor]\n"
                        "                [--seed N] [--log] [--min-yield X] [--final LO HI] [--max-changes N]\n");
        return 2;
    }
    f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "error: cannot open %s (make it with experiments/gain_sweep.py --model)\n", path);
        return 2;
    }
    while (fgets(line, sizeof(line), f))
    {
        int d, w;
        win_t x;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%d %d %lf %u %u %u %u %u %lf %lf %u %u", &d, &w, &x.secs, &x.ok, &x.es_ok, &x.es_lost, &x.loud,
                   &x.pos, &x.noise, &x.clip, &x.sig, &x.delta) != 12)
            continue;
        if (d < DB_LO || d > DB_HI || model_n[d - DB_LO] >= MAXWIN)
            continue;
        model[d - DB_LO][model_n[d - DB_LO]++] = x;
    }
    fclose(f);
    for (i = DB_LO; i <= DB_HI; i++)
        if (model_n[i - DB_LO] == 0)
        {
            fprintf(stderr, "error: %s has no windows for %+d dB\n", path, i);
            return 2;
        }

    {
        gain_ctl_t g;
        gain_obs_t o;
        double t = 0, win_t0 = 0, sig_w = 0, delta_w = 0, noise_w = 0, clip_w = 0;
        double got_pos = 0, best_fixed[NGAIN] = {0}, time_at[NGAIN] = {0};
        int changes = 0, settle = 0, lowest, highest, best = FLOOR_IDX;
        double mp[NGAIN];
        char log[640];

        for (i = 0; i < NGAIN; i++)
            mp[i] = mean_pos(i);
        gain_ctl_init(&g, GAINS, NGAIN, FLOOR_IDX);
        if (start_floor)
        {
            g.idx = FLOOR_IDX;
            gain_ctl_restart(&g, false);
        }
        lowest = highest = g.idx;
        memset(&o, 0, sizeof(o));

        while (t < hours * 3600.0)
        {
            double tr = traffic(t / 3600.0);
            int d = model_db(g.idx);
            win_t x;
            memset(&x, 0, sizeof(x));
            x.secs = SLICE_S;
            if (d >= DB_LO)
            {
                if (d > DB_HI)
                    d = DB_HI;
                x = model[d - DB_LO][(int)(rnd() * model_n[d - DB_LO])];
            }
            else
            {
                x.noise = model[0][0].noise; // under one count either way
            }
            x.ok = thin(x.ok, tr);
            x.es_ok = thin(x.es_ok, tr);
            x.es_lost = thin(x.es_lost, tr);
            x.loud = thin(x.loud, tr);
            if (x.loud > x.ok)
                x.loud = x.ok;
            x.pos = thin(x.pos, tr);
            t += x.secs;
            time_at[g.idx] += x.secs;
            got_pos += x.pos;
            for (i = FLOOR_IDX; i < NGAIN; i++)
                best_fixed[i] += mp[i] * tr * x.secs;

            if (settle)
            {
                // the slice after a gain change is not counted (class_driver.c: GAIN_SETTLE_US)
                settle = 0;
                memset(&o, 0, sizeof(o));
                sig_w = delta_w = noise_w = clip_w = 0;
                win_t0 = t;
                continue;
            }
            o.ok += x.ok;
            o.es_ok += x.es_ok;
            o.es_lost += x.es_lost;
            o.loud += x.loud;
            o.pos += x.pos;
            sig_w += (double)x.sig * x.ok;
            delta_w += (double)x.delta * x.ok;
            noise_w += x.noise * x.secs;
            clip_w += x.clip * x.secs;

            {
                double el = t - win_t0;
                int due = el >= gain_ctl_window_us(&g) / 1e6 ||
                          (g.phase == 1 && el >= GAIN_P1_MIN_US / 1e6 && o.ok >= GAIN_P1_MSGS);
                int old = g.idx, idx;
                if (!due)
                    continue;
                o.secs = (float)el;
                o.avg_signal = o.ok ? (uint32_t)(sig_w / o.ok) : 0;
                o.avg_delta = o.ok ? (uint32_t)(delta_w / o.ok) : 0;
                o.noise = (float)(noise_w / el);
                o.clip = (float)(clip_w / el);
                idx = gain_ctl_window(&g, &o, log, sizeof(log));
                if (show_log && log[0])
                {
                    char *p = log;
                    while (p && *p)
                    {
                        char *nl = strchr(p, '\n');
                        if (nl)
                            *nl = 0;
                        printf("[%8.0f s] %s\n", t, p);
                        p = nl ? nl + 1 : NULL;
                    }
                }
                if (idx != old)
                {
                    changes++;
                    settle = 1;
                    if (idx < lowest)
                        lowest = idx;
                    if (idx > highest)
                        highest = idx;
                }
                memset(&o, 0, sizeof(o));
                sig_w = delta_w = noise_w = clip_w = 0;
                win_t0 = t;
            }
        }

        for (i = FLOOR_IDX; i < NGAIN; i++)
            if (best_fixed[i] > best_fixed[best])
                best = i;
        {
            double yield = best_fixed[best] > 0 ? got_pos / best_fixed[best] : 1.0;
            int fail = 0;
            printf("site %+d dB, %.0f h, traffic %s: ended at %.1f dB (phase %d), %d changes, range %.1f-%.1f dB; "
                   "yield %.1f%% of the best fixed gain (%.1f dB)\n",
                   site_db, hours, traffic_mode == 1 ? "day" : traffic_mode == 2 ? "night" : "flat", GAINS[g.idx] / 10.0,
                   g.phase, changes, GAINS[lowest] / 10.0, GAINS[highest] / 10.0, yield * 100.0, GAINS[best] / 10.0);
            printf("  time at each gain:");
            for (i = FLOOR_IDX; i < NGAIN; i++)
                if (time_at[i] > 0)
                    printf(" %.1f dB %.0f%%", GAINS[i] / 10.0, 100.0 * time_at[i] / t);
            printf("\n");
            if (min_yield >= 0 && yield < min_yield)
            {
                printf("  FAIL: yield %.3f under %.3f\n", yield, min_yield);
                fail = 1;
            }
            if (GAINS[g.idx] / 10.0 < final_lo - 1e-6 || GAINS[g.idx] / 10.0 > final_hi + 1e-6)
            {
                printf("  FAIL: final gain outside %.1f..%.1f dB\n", final_lo, final_hi);
                fail = 1;
            }
            if (max_changes >= 0 && changes > max_changes)
            {
                printf("  FAIL: %d gain changes, more than %d\n", changes, max_changes);
                fail = 1;
            }
            return fail;
        }
    }
}
