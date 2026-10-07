#include "dj_link_clock.h"

#include <math.h>
#include <string.h>

void dj_link_clock_reset(dj_link_clock_t *c)
{
    memset(c, 0, sizeof(*c));
}

void dj_link_clock_bank_reset(dj_link_clock_bank_t *b)
{
    memset(b, 0, sizeof(*b));
}

/* The period is the packet's own tempo (exact, and the best offset estimate
 * follows from it: mean of the arrival times minus index * period, error
 * sigma / sqrt(n)). The least-squares slope over the window only replaces it
 * when it differs by more than three standard errors, at most
 * DJ_LINK_CLOCK_SLOPE_TOL: a player whose crystal runs off its announced
 * tempo by a few hundred ppm. Times are doubles relative to the newest beat
 * so 64-bit microseconds lose nothing. */
#define DJ_LINK_CLOCK_SLOPE_MIN_BEATS 12u
#define DJ_LINK_CLOCK_SLOPE_SIGMAS    3.0

static void fit(dj_link_clock_t *c)
{
    const uint32_t n = c->n;
    const int32_t last = c->idx[n - 1u];
    const int64_t t_last = c->t_us[n - 1u];
    const double hint = (double)c->hint_us;
    double period = hint;

    if (n >= DJ_LINK_CLOCK_SLOPE_MIN_BEATS) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            const double x = (double)(c->idx[i] - last);
            const double y = (double)(c->t_us[i] - t_last);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double sxx_c = sxx - sx * sx / (double)n;
        if (sxx_c > 0.0) {
            const double b = (sxy - sx * sy / (double)n) / sxx_c;
            const double a = (sy - b * sx) / (double)n;
            double ss = 0.0;
            for (uint32_t i = 0; i < n; i++) {
                const double r = (double)(c->t_us[i] - t_last) -
                                 (a + b * (double)(c->idx[i] - last));
                ss += r * r;
            }
            const double se = sqrt(ss / (double)(n - 2u) / sxx_c);
            const double dev = fabs(b - hint);
            if (dev > DJ_LINK_CLOCK_SLOPE_SIGMAS * se && dev <= hint * (double)DJ_LINK_CLOCK_SLOPE_TOL) {
                period = b;
            }
        }
    }

    double off = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        off += (double)(c->t_us[i] - t_last) - period * (double)(c->idx[i] - last);
    }
    off /= (double)n;

    double ss = 0.0;
    if (n >= DJ_LINK_CLOCK_MIN_LOCK) {
        for (uint32_t i = 0; i < n; i++) {
            const double r = (double)(c->t_us[i] - t_last) -
                             (off + period * (double)(c->idx[i] - last));
            ss += r * r;
        }
    }
    c->anchor_us = t_last + (int64_t)llround(off);
    c->period_us = (uint32_t)llround(period);
    c->rms_us = n >= DJ_LINK_CLOCK_MIN_LOCK ? (float)sqrt(ss / (double)n) : 0.0f;
    c->locked = n >= DJ_LINK_CLOCK_MIN_LOCK;
}

static void restart(dj_link_clock_t *c, int64_t rx_us, uint8_t bar, uint32_t hint_us)
{
    dj_link_clock_reset(c);
    c->n = 1u;
    c->t_us[0] = rx_us;
    c->idx[0] = 0;
    c->last_bar = bar;
    c->hint_us = hint_us;
    c->accepted = 1u;
    fit(c);
}

void dj_link_clock_beat(dj_link_clock_t *c, int64_t rx_us, uint8_t beat_in_bar,
                        uint32_t hint_period_us)
{
    if (!c || hint_period_us == 0u) {
        return;
    }
    const uint8_t bar = beat_in_bar >= 1u && beat_in_bar <= 4u ? beat_in_bar : 0u;
    if (c->n == 0u) {
        restart(c, rx_us, bar, hint_period_us);
        return;
    }
    /* A tempo change (pitch fader moving) invalidates the older beats. */
    const double change = fabs((double)hint_period_us - (double)c->hint_us) / (double)c->hint_us;
    if (change > (double)DJ_LINK_CLOCK_TEMPO_TOL) {
        restart(c, rx_us, bar, hint_period_us);
        return;
    }
    const int64_t dt = rx_us - c->t_us[c->n - 1u];
    const double ref = c->locked ? (double)c->period_us : (double)hint_period_us;
    const double beats = (double)dt / ref;
    const int32_t steps = (int32_t)floor(beats + 0.5);
    if (steps < 1) {
        /* Two packets for one beat, or one arriving early: not a new beat. */
        if (++c->outliers > DJ_LINK_CLOCK_OUTLIER_MAX) {
            restart(c, rx_us, bar, hint_period_us);
        }
        return;
    }
    if (steps > DJ_LINK_CLOCK_GAP_BEATS) {
        restart(c, rx_us, bar, hint_period_us);   /* stopped, paused or lost */
        return;
    }
    if (bar != 0u && c->last_bar != 0u) {
        const uint8_t expect = (uint8_t)((c->last_bar - 1u + (uint32_t)steps) % 4u + 1u);
        if (expect != bar) {
            restart(c, rx_us, bar, hint_period_us);   /* the player jumped */
            return;
        }
    }
    /* Jitter beyond a fraction of a beat from where the line says. */
    const int32_t idx_new = c->idx[c->n - 1u] + steps;
    const double predicted = (double)c->anchor_us +
                             (double)(idx_new - c->idx[c->n - 1u]) * (double)c->period_us;
    if (fabs((double)rx_us - predicted) > ref * (double)DJ_LINK_CLOCK_OUTLIER_FRAC) {
        if (++c->outliers > DJ_LINK_CLOCK_OUTLIER_MAX) {
            restart(c, rx_us, bar, hint_period_us);
        }
        return;
    }
    c->outliers = 0u;

    if (c->n == DJ_LINK_CLOCK_WINDOW) {
        memmove(&c->t_us[0], &c->t_us[1], (DJ_LINK_CLOCK_WINDOW - 1u) * sizeof(c->t_us[0]));
        memmove(&c->idx[0], &c->idx[1], (DJ_LINK_CLOCK_WINDOW - 1u) * sizeof(c->idx[0]));
        c->n--;
    }
    c->t_us[c->n] = rx_us;
    c->idx[c->n] = idx_new;
    c->n++;
    c->last_bar = bar != 0u ? bar : (c->last_bar != 0u ? (uint8_t)((c->last_bar - 1u + (uint32_t)steps) % 4u + 1u) : 0u);
    c->hint_us = hint_period_us;
    c->accepted++;
    fit(c);
}

void dj_link_clock_bank_beat(dj_link_clock_bank_t *b, uint8_t player, int64_t rx_us,
                             uint8_t beat_in_bar, uint32_t hint_period_us)
{
    if (b && player >= 1u && player <= DJ_LINK_MAX_PEERS) {
        dj_link_clock_beat(&b->c[player - 1u], rx_us, beat_in_bar, hint_period_us);
    }
}

bool dj_link_clock_get(const dj_link_clock_t *c, dj_link_clock_fit_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!c || c->n == 0u) {
        return false;
    }
    out->valid = true;
    out->locked = c->locked;
    out->anchor_us = c->anchor_us;
    out->period_us = c->period_us;
    out->beat_in_bar = c->last_bar;
    out->rms_us = c->rms_us;
    out->beats = c->n;
    return true;
}

bool dj_link_clock_bank_get(const dj_link_clock_bank_t *b, uint8_t player,
                            dj_link_clock_fit_t *out)
{
    if (!b || player < 1u || player > DJ_LINK_MAX_PEERS) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    return dj_link_clock_get(&b->c[player - 1u], out);
}
