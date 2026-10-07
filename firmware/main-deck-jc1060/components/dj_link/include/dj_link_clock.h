#pragma once
//
// dj_link_clock (v339) - the beat clock of one DJ Link player, estimated from
// the arrival times of its beat packets (port 50001, type 0x28).
//
// One packet's arrival time is the beat time plus network and scheduling
// jitter (a millisecond or more), and the old clock took the last packet as
// the anchor. Here every packet, stamped in microseconds in the lwIP receive
// callback, goes into a sliding window of beats; the beat index is recovered
// from the time between packets and the bar position, so a dropped packet
// leaves a gap in the index, not a wrong phase. A least-squares line through
// (index, time) checks the period against the packet's own BPM (the player's
// effective tempo, exact): the BPM is used unless the line differs by more
// than its noise allows, and the offset is the mean of the window. A tempo
// change, a jump of the bar or a gap restart the window. Pure: no clock, no heap,
// host tested.
//
#include <stdbool.h>
#include <stdint.h>

#include "dj_link_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_CLOCK_WINDOW      16u     /* beats kept */
#define DJ_LINK_CLOCK_MIN_LOCK    4u      /* beats before the line is trusted */
#define DJ_LINK_CLOCK_GAP_BEATS   4       /* a longer silence restarts the window */
#define DJ_LINK_CLOCK_TEMPO_TOL   0.003f  /* hint change (fraction) that restarts it */
#define DJ_LINK_CLOCK_SLOPE_TOL   0.003f  /* most the line may differ from the hint */
#define DJ_LINK_CLOCK_OUTLIER_FRAC 0.10f  /* of a beat: a packet this far off is skipped */
#define DJ_LINK_CLOCK_OUTLIER_MAX 2u      /* skipped in a row before restarting */

typedef struct {
    uint32_t n;                           /* beats in the window */
    int64_t  t_us[DJ_LINK_CLOCK_WINDOW];  /* arrival times, oldest first */
    int32_t  idx[DJ_LINK_CLOCK_WINDOW];   /* beat index of each */
    uint8_t  last_bar;                    /* beat in bar of the newest, 0 = unknown */
    uint32_t hint_us;                     /* period of the newest packet's BPM */
    uint8_t  outliers;                    /* packets skipped in a row */
    uint32_t accepted;                    /* packets used since the last restart */
    /* The fit, valid when n >= 1. */
    int64_t  anchor_us;                   /* estimated time of the newest beat */
    uint32_t period_us;
    float    rms_us;                      /* residual of the line, 0 before MIN_LOCK */
    bool     locked;                      /* n >= MIN_LOCK */
} dj_link_clock_t;

typedef struct {
    dj_link_clock_t c[DJ_LINK_MAX_PEERS]; /* by device number - 1 */
} dj_link_clock_bank_t;

typedef struct {
    bool     valid;
    bool     locked;
    int64_t  anchor_us;                   /* newest beat, local clock (esp_timer us) */
    uint32_t period_us;
    uint8_t  beat_in_bar;                 /* of that beat, 0 = unknown */
    float    rms_us;
    uint32_t beats;                       /* window size */
} dj_link_clock_fit_t;

void dj_link_clock_reset(dj_link_clock_t *c);
void dj_link_clock_bank_reset(dj_link_clock_bank_t *b);

/* One beat packet from `player` received at rx_us. hint_period_us is the
 * packet's effective beat length (60e6 / effective BPM), 0 = unusable (the
 * packet is dropped). beat_in_bar 1..4, 0 = unknown. */
void dj_link_clock_beat(dj_link_clock_t *c, int64_t rx_us, uint8_t beat_in_bar,
                        uint32_t hint_period_us);
/* Same on the bank, player 1..6; others ignored. */
void dj_link_clock_bank_beat(dj_link_clock_bank_t *b, uint8_t player, int64_t rx_us,
                             uint8_t beat_in_bar, uint32_t hint_period_us);

bool dj_link_clock_get(const dj_link_clock_t *c, dj_link_clock_fit_t *out);
bool dj_link_clock_bank_get(const dj_link_clock_bank_t *b, uint8_t player,
                            dj_link_clock_fit_t *out);

#ifdef __cplusplus
}
#endif
