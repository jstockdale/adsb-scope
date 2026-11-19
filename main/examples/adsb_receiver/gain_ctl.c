// gain_ctl.c - adaptive tuner gain from signal levels and yield. See gain_ctl.h.

#include "gain_ctl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Phase 2 bookkeeping, as in the loop this one replaces (class_driver.c)
#define P2_EMA_ALPHA 0.3f
#define P2_COOLDOWN 2           // windows to let a new gain settle before judging it
#define P2_THROUGHPUT_DROP 0.15f // a step that costs more than this is taken back
#define P2_SIG_CHANGE 0.40f     // ... unless the signal level moved this much: traffic changed, not us
#define P2_STRIKES 2            // failed steps in one direction before it is fenced off
#define P2_FENCE_MAX_WIN 20     // a ceiling / floor lasts this many windows at most
#define P2_FENCE_CALM_WIN 5     // ... or this many with the opposite end complaining
#define P2_MIN_OK 3u            // decoded frames a window needs to move the averages
#define PROFILE_ALPHA 0.1f
#define PROFILE_MIN_WIN 3
#define PROFILE_MIN_DELTA 0.25f

typedef struct
{
    char *p;
    size_t left;
} out_t;

static void say(out_t *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (o->left < 2)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= o->left)
        n = (int)o->left - 1;
    o->p += n;
    o->left -= (size_t)n;
}

float gain_ctl_headroom_db(float noise)
{
    return noise > 0 ? 20.0f * log10f(127.5f / noise) : 0.0f;
}

float gain_ctl_err(const gain_obs_t *o)
{
    uint32_t seen = o->es_ok + o->es_lost;
    return seen >= 10 ? (float)o->es_lost / (float)seen : -1.0f;
}

static float db(const gain_ctl_t *g, int idx)
{
    return g->tab[idx] / 10.0f;
}

// The step nearest to (current - tenths), at least one step down, not under the floor.
static int idx_down(const gain_ctl_t *g, int cur, int tenths)
{
    int target = (int)g->tab[cur] - tenths, best = g->floor_idx, i;
    int best_dist;
    if (cur <= g->floor_idx)
        return cur;
    if (target < (int)g->tab[g->floor_idx])
        target = g->tab[g->floor_idx];
    best_dist = abs((int)g->tab[best] - target);
    for (i = g->floor_idx + 1; i < cur; i++)
    {
        int d = abs((int)g->tab[i] - target);
        if (d <= best_dist)
        {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

// The step nearest to (current + tenths), at least one step up, not over the top.
static int idx_up(const gain_ctl_t *g, int cur, int tenths)
{
    int max = g->count - 1, target = (int)g->tab[cur] + tenths, best = max, i;
    int best_dist;
    if (cur >= max)
        return cur;
    if (target > (int)g->tab[max])
        target = g->tab[max];
    best_dist = abs((int)g->tab[best] - target);
    for (i = max - 1; i > cur; i--)
    {
        int d = abs((int)g->tab[i] - target);
        if (d <= best_dist)
        {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

static void fences_clear(gain_ctl_t *g)
{
    g->ceiling = g->count;
    g->ceiling_strikes = 0;
    g->ceiling_age = 0;
    g->ceiling_calm = 0;
    g->floor = -1;
    g->floor_strikes = 0;
    g->floor_age = 0;
    g->floor_calm = 0;
}

void gain_ctl_restart(gain_ctl_t *g, bool from_max)
{
    g->phase = 1;
    g->stable = 0;
    g->ema_valid = false;
    g->pos_ema = 0;
    g->err_ema = 0;
    g->loud_ema = 0;
    g->head_ema = 0;
    g->pre_pos = 0;
    g->pre_sig = 0;
    g->step_dir = 0;
    g->cooldown = 0;
    fences_clear(g);
    if (from_max)
    {
        g->idx = g->count - 1;
        memset(g->profile, 0, sizeof(g->profile));
        memset(g->profile_n, 0, sizeof(g->profile_n));
    }
    g->prev_idx = g->idx;
}

void gain_ctl_init(gain_ctl_t *g, const uint16_t *tab_tenths, int count, int floor_idx)
{
    memset(g, 0, sizeof(*g));
    if (count > GAIN_CTL_MAX_STEPS)
        count = GAIN_CTL_MAX_STEPS;
    g->tab = tab_tenths;
    g->count = count;
    g->floor_idx = floor_idx < 0 ? 0 : (floor_idx >= count ? count - 1 : floor_idx);
    g->loud_last = -1.0f;
    gain_ctl_restart(g, true);
}

uint32_t gain_ctl_window_us(const gain_ctl_t *g)
{
    return g->phase == 1 ? GAIN_P1_WINDOW_US : GAIN_P2_WINDOW_US;
}

// "loud=4% noise=3.2 headroom=32dB clip=0.03%" from the last window
static void say_levels(out_t *o, const gain_ctl_t *g)
{
    if (g->loud_last >= 0)
        say(o, "loud=%.0f%%", (double)(g->loud_last * 100.0f));
    else
        say(o, "loud=n/a");
    if (g->noise_last > 0)
        say(o, " noise=%.1f headroom=%.0fdB", (double)g->noise_last, (double)g->head_last);
    else
        say(o, " noise=n/a");
    say(o, " clip=%.2f%%", (double)(g->clip_last * 100.0f));
}

// ---- Phase 1: put the gain where neither end complains ------------------

static void phase1(gain_ctl_t *g, const gain_obs_t *o, float pos_rate, out_t *out)
{
    const int old = g->idx, max = g->count - 1;
    const float head = g->head_last, loud = g->loud_last;
    const float err = g->err_last;
    const char *why = NULL;
    int next = old;

    if (head > 0 && head < GAIN_HEAD_MIN && old > g->floor_idx)
    {
        // The noise itself is near the rails. Aim for GAIN_HEAD_AIM.
        float drop = GAIN_HEAD_AIM - head;
        if (drop > 9.0f)
            drop = 9.0f;
        next = idx_down(g, old, (int)(drop * 10.0f + 0.5f));
        why = "noise near full scale";
    }
    else if (loud >= GAIN_LOUD_HOT && old > g->floor_idx)
    {
        next = idx_down(g, old, loud >= GAIN_LOUD_VERY_HOT ? 60 : 30);
        why = "too many frames at full scale";
    }
    else if (old < max && loud < GAIN_LOUD_LOW && (head > 0 ? head > GAIN_HEAD_UP : o->ok == 0))
    {
        // Nothing is loud (or nothing is heard at all) and there is room:
        // more gain. With a noise figure, by what the headroom allows, 3 dB
        // at a time (6 while the ADC is starved); without one, a step.
        if (head > 0)
        {
            float lim = head > GAIN_HEAD_COLD ? 6.0f : 3.0f;
            float raise = head - GAIN_HEAD_KEEP;
            if (raise > lim)
                raise = lim;
            next = idx_up(g, old, (int)(raise * 10.0f + 0.5f));
            why = head > GAIN_HEAD_COLD ? "noise under one ADC step" : "headroom to spare, nothing loud";
        }
        else
        {
            next = old + 1;
            why = "no messages";
        }
    }

    if (next != old)
    {
        g->idx = next;
        g->stable = 0;
        say(out, "GAIN: [P1] %s: %.1f \xE2\x86\x92 %.1f dB (%+.1f dB, %s; err=%.0f%%, pos=%.1f/s, ok=%lu, sig=%lu delta=%lu, ",
            next < old ? "Down" : "Up", (double)db(g, old), (double)db(g, next), (double)(db(g, next) - db(g, old)), why,
            (double)(err * 100.0f), (double)pos_rate, (unsigned long)o->ok, (unsigned long)o->avg_signal,
            (unsigned long)o->avg_delta);
        say_levels(out, g);
        say(out, ")\n");
        return;
    }

    if (++g->stable >= GAIN_P1_STABLE)
    {
        g->phase = 2;
        g->ema_valid = true;
        g->pos_ema = pos_rate;
        g->err_ema = err;
        g->loud_ema = loud >= 0 ? loud : 0.0f;
        g->head_ema = head;
        g->cooldown = 0;
        g->step_dir = 0;
        fences_clear(g);
        say(out, "GAIN: Converged at %.1f dB (err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu, ",
            (double)db(g, g->idx), (double)(err * 100.0f), (double)pos_rate, (double)o->range_nm,
            (unsigned long)o->avg_signal, (unsigned long)o->avg_delta);
        say_levels(out, g);
        say(out, ") \xE2\x86\x92 steady-state\n");
    }
    else
    {
        say(out, "GAIN: [P1] Holding %.1f dB (err=%.0f%%, pos=%.1f/s, ok=%lu, sig=%lu delta=%lu, ",
            (double)db(g, g->idx), (double)(err * 100.0f), (double)pos_rate, (unsigned long)o->ok,
            (unsigned long)o->avg_signal, (unsigned long)o->avg_delta);
        say_levels(out, g);
        say(out, ")\n");
    }
}

// ---- Phase 2: hold, and explore one step at a time ---------------------

static void step_to(gain_ctl_t *g, const gain_obs_t *o, int next)
{
    g->pre_pos = g->pos_ema;
    g->pre_sig = o->avg_signal;
    g->prev_idx = g->idx;
    g->step_dir = next > g->idx ? 1 : -1;
    g->idx = next;
    g->cooldown = P2_COOLDOWN;
}

static void phase2(gain_ctl_t *g, const gain_obs_t *o, float pos_rate, float err, out_t *out)
{
    const int max = g->count - 1;
    const float head = g->head_last, loud = g->loud_last;
    const int old = g->idx;

    // Averages. Yield and error rate move only when something was decoded,
    // so a quiet hour does not teach the loop that this gain yields
    // nothing; the loud share needs enough frames to be a share at all;
    // the noise needs no traffic.
    if (o->ok >= P2_MIN_OK)
    {
        g->pos_ema = P2_EMA_ALPHA * pos_rate + (1.0f - P2_EMA_ALPHA) * g->pos_ema;
        if (err >= 0)
            g->err_ema = P2_EMA_ALPHA * err + (1.0f - P2_EMA_ALPHA) * g->err_ema;
    }
    if (loud >= 0)
        g->loud_ema = P2_EMA_ALPHA * loud + (1.0f - P2_EMA_ALPHA) * g->loud_ema;
    if (head > 0)
        g->head_ema = g->head_ema > 0 ? P2_EMA_ALPHA * head + (1.0f - P2_EMA_ALPHA) * g->head_ema : head;
    g->err_last = g->err_ema;
    g->pos_last = g->pos_ema;

    // What this gain has yielded over the hours
    if (o->ok >= P2_MIN_OK)
    {
        if (g->profile_n[old] == 0)
            g->profile[old] = pos_rate;
        else
            g->profile[old] = PROFILE_ALPHA * pos_rate + (1.0f - PROFILE_ALPHA) * g->profile[old];
        if (g->profile_n[old] < 65535)
            g->profile_n[old]++;
    }

    // Overload: not something to explore out of one step at a time. (The
    // loud share must be this window's too: an average left over from the
    // last traffic of the evening is no reason to act in the night.)
    if ((loud >= 0 && g->loud_ema >= GAIN_LOUD_VERY_HOT) || (g->head_ema > 0 && g->head_ema < GAIN_HEAD_MIN))
    {
        say(out, "GAIN: Overload (");
        say_levels(out, g);
        say(out, ") \xE2\x86\x92 returning to fast convergence\n");
        gain_ctl_restart(g, false);
        return;
    }
    // Starved: the same, the other way.
    if (g->head_ema > GAIN_HEAD_COLD && old < max && g->loud_ema < GAIN_LOUD_LOW && g->step_dir == 0 &&
        g->ceiling >= g->count)
    {
        say(out, "GAIN: Noise under one ADC step (");
        say_levels(out, g);
        say(out, ") \xE2\x86\x92 returning to fast convergence\n");
        gain_ctl_restart(g, false);
        return;
    }

    // Fences age out, or fall early when the far end complains for a while.
    if (g->ceiling < g->count)
    {
        bool cold = g->head_ema > GAIN_HEAD_COLD - 3.0f;
        g->ceiling_calm = cold ? g->ceiling_calm + 1 : 0;
        if (++g->ceiling_age >= P2_FENCE_MAX_WIN || g->ceiling_calm >= P2_FENCE_CALM_WIN)
        {
            say(out, "GAIN: Ceiling %s \xE2\x86\x92 will retry higher\n",
                g->ceiling_age >= P2_FENCE_MAX_WIN ? "expired" : "cleared (little noise at the ADC)");
            g->ceiling = g->count;
            g->ceiling_strikes = 0;
            g->ceiling_age = 0;
            g->ceiling_calm = 0;
        }
    }
    if (g->floor >= 0)
    {
        bool hot = g->loud_ema >= GAIN_LOUD_HOT;
        g->floor_calm = hot ? g->floor_calm + 1 : 0;
        if (++g->floor_age >= P2_FENCE_MAX_WIN || g->floor_calm >= P2_FENCE_CALM_WIN)
        {
            say(out, "GAIN: Floor %s \xE2\x86\x92 will retry lower\n",
                g->floor_age >= P2_FENCE_MAX_WIN ? "expired" : "cleared (frames at full scale)");
            g->floor = -1;
            g->floor_strikes = 0;
            g->floor_age = 0;
            g->floor_calm = 0;
        }
    }

    if (g->cooldown > 0)
    {
        g->cooldown--;
        say(out, "GAIN: [P2] Settling: %.1f dB, EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu (cooldown %d), ",
            (double)db(g, old), (double)(g->err_ema * 100.0f), (double)g->pos_ema, (double)o->range_nm,
            (unsigned long)o->avg_signal, (unsigned long)o->avg_delta, g->cooldown);
        say_levels(out, g);
        say(out, "\n");
        return;
    }

    // Judge the last step by what it did to the yield.
    if (g->step_dir != 0)
    {
        if (g->pre_pos > 0.0f)
        {
            float change = (g->pos_ema - g->pre_pos) / g->pre_pos;
            if (change < -P2_THROUGHPUT_DROP)
            {
                bool shifted = false;
                if (g->pre_sig > 0 && o->ok >= P2_MIN_OK)
                    shifted = fabsf((float)o->avg_signal - (float)g->pre_sig) / (float)g->pre_sig > P2_SIG_CHANGE;
                if (shifted)
                {
                    // The signal level moved a lot: other aircraft, not this step.
                    say(out, "GAIN: [P2] Holding %.1f dB \xE2\x80\x94 throughput %+.0f%% but traffic shifted (sig %lu \xE2\x86\x92 %lu, no strike)\n",
                        (double)db(g, old), (double)(change * 100.0f), (unsigned long)g->pre_sig,
                        (unsigned long)o->avg_signal);
                    g->step_dir = 0;
                    g->cooldown = P2_COOLDOWN;
                    return;
                }
                {
                    const char *sfx;
                    if (g->step_dir > 0)
                    {
                        if (++g->ceiling_strikes >= P2_STRIKES)
                        {
                            g->ceiling = old;
                            g->ceiling_age = 0;
                            g->ceiling_calm = 0;
                        }
                        sfx = g->ceiling < g->count ? ", ceiling set" : ", ceiling strike";
                    }
                    else
                    {
                        if (++g->floor_strikes >= P2_STRIKES)
                        {
                            g->floor = old;
                            g->floor_age = 0;
                            g->floor_calm = 0;
                        }
                        sfx = g->floor >= 0 ? ", floor set" : ", floor strike";
                    }
                    g->idx = g->prev_idx;
                    g->step_dir = 0;
                    g->cooldown = P2_COOLDOWN;
                    say(out, "GAIN: [P2] Reverted: %.1f \xE2\x86\x92 %.1f dB (throughput %+.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu%s)\n",
                        (double)db(g, old), (double)db(g, g->idx), (double)(change * 100.0f), (double)g->pos_ema,
                        (double)o->range_nm, (unsigned long)o->avg_signal, (unsigned long)o->avg_delta, sfx);
                    return;
                }
            }
            g->ceiling_strikes = 0;
            g->floor_strikes = 0;
            say(out, "GAIN: [P2] Confirmed: %.1f dB (throughput %+.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu)\n",
                (double)db(g, old), (double)(change * 100.0f), (double)g->pos_ema, (double)o->range_nm,
                (unsigned long)o->avg_signal, (unsigned long)o->avg_delta);
            g->step_dir = 0;
            g->cooldown = P2_COOLDOWN;
            return;
        }
        g->step_dir = 0; // nothing was being decoded before the step: nothing to judge by
    }

    // A neighbour that has done clearly better over the hours?
    {
        int best = -1;
        float best_rate = g->pos_ema, need = g->pos_ema * 1.05f;
        if (need < g->pos_ema + PROFILE_MIN_DELTA)
            need = g->pos_ema + PROFILE_MIN_DELTA;
        if (old + 1 <= max && old + 1 < g->ceiling && g->profile_n[old + 1] >= PROFILE_MIN_WIN &&
            g->profile[old + 1] > need)
        {
            best = old + 1;
            best_rate = g->profile[best];
        }
        if (old - 1 >= g->floor_idx && old - 1 > g->floor && g->profile_n[old - 1] >= PROFILE_MIN_WIN &&
            g->profile[old - 1] > need && g->profile[old - 1] > best_rate)
        {
            best = old - 1;
            best_rate = g->profile[best];
        }
        if (best >= 0 && o->ok >= P2_MIN_OK)
        {
            step_to(g, o, best);
            say(out, "GAIN: [P2] Probing: %.1f \xE2\x86\x92 %.1f dB (profile: %.1f/s vs current %.1f/s, sig=%lu delta=%lu)\n",
                (double)db(g, old), (double)db(g, best), (double)best_rate, (double)g->pos_ema,
                (unsigned long)o->avg_signal, (unsigned long)o->avg_delta);
            return;
        }
    }

    // Explore by the levels: down while too much is loud, up while nothing
    // is and headroom remains.
    if (loud >= 0 && g->loud_ema > GAIN_LOUD_HIGH && old > g->floor_idx && old - 1 > g->floor)
    {
        step_to(g, o, old - 1);
        say(out, "GAIN: [P2] Exploring down: %.1f \xE2\x86\x92 %.1f dB (EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu, ",
            (double)db(g, old), (double)db(g, g->idx), (double)(g->err_ema * 100.0f), (double)g->pos_ema,
            (double)o->range_nm, (unsigned long)o->avg_signal, (unsigned long)o->avg_delta);
        say_levels(out, g);
        say(out, ")\n");
        return;
    }
    if (g->loud_ema < GAIN_LOUD_LOW && g->head_ema > GAIN_HEAD_KEEP + 1.0f && old < max && old + 1 < g->ceiling)
    {
        float raise = g->head_ema - GAIN_HEAD_KEEP;
        int next;
        if (raise > 3.0f)
            raise = 3.0f;
        next = idx_up(g, old, (int)(raise * 10.0f + 0.5f));
        if (next >= g->ceiling)
            next = g->ceiling - 1;
        if (next > old)
        {
            step_to(g, o, next);
            say(out, "GAIN: [P2] Exploring up: %.1f \xE2\x86\x92 %.1f dB (%+.1f dB, EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu, ",
                (double)db(g, old), (double)db(g, next), (double)(db(g, next) - db(g, old)),
                (double)(g->err_ema * 100.0f), (double)g->pos_ema, (double)o->range_nm,
                (unsigned long)o->avg_signal, (unsigned long)o->avg_delta);
            say_levels(out, g);
            say(out, ")\n");
            return;
        }
    }

    {
        const char *bounds = "";
        if (g->ceiling < g->count && g->floor >= 0)
            bounds = ", bounds: ceiling+floor set";
        else if (g->ceiling < g->count)
            bounds = ", bounds: ceiling set";
        else if (g->floor >= 0)
            bounds = ", bounds: floor set";
        say(out, "GAIN: [P2] Steady: %.1f dB, EMA err=%.0f%%, pos=%.1f/s, range=%.0fnm, sig=%lu delta=%lu, ",
            (double)db(g, old), (double)(g->err_ema * 100.0f), (double)g->pos_ema, (double)o->range_nm,
            (unsigned long)o->avg_signal, (unsigned long)o->avg_delta);
        say_levels(out, g);
        say(out, "%s\n", bounds);
    }
}

int gain_ctl_window(gain_ctl_t *g, const gain_obs_t *o, char *log, size_t log_size)
{
    out_t out = {log, log_size};
    float secs = o->secs > 0.5f ? o->secs : 0.5f;
    float pos_rate = (float)o->pos / secs;
    float err = gain_ctl_err(o);

    if (log_size)
        log[0] = '\0';

    g->noise_last = o->noise;
    g->head_last = gain_ctl_headroom_db(o->noise);
    g->clip_last = o->clip;
    g->loud_last = o->ok >= GAIN_LOUD_MIN_OK ? (float)o->loud / (float)o->ok : -1.0f;
    g->sig_last = o->avg_signal;
    g->delta_last = o->avg_delta;

    if (g->phase == 1)
    {
        if (err >= 0)
            g->err_last = err;
        g->pos_last = pos_rate;
        phase1(g, o, pos_rate, &out);
    }
    else
    {
        phase2(g, o, pos_rate, err, &out);
    }
    return g->idx;
}
