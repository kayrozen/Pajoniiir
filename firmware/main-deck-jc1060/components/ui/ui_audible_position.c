#include "ui_audible_position.h"

#include <string.h>

void ui_audible_position_init(ui_audible_position_t *ap)
{
    if (ap) {
        memset(ap, 0, sizeof(*ap));
    }
}

static const ui_audible_position_sample_t *sample_back(const ui_audible_position_t *ap, uint8_t back)
{
    /* back 0 = newest. */
    uint8_t slot = (uint8_t)((ap->head + UI_AUDIBLE_POSITION_SAMPLES - 1u - back) %
                             UI_AUDIBLE_POSITION_SAMPLES);
    return &ap->sample[slot];
}

uint32_t ui_audible_position_update(ui_audible_position_t *ap,
                                    uint32_t position_ms,
                                    uint64_t now_us,
                                    uint32_t latency_us)
{
    if (!ap) {
        return position_ms;
    }

    /* Same timestamp (two updates in one tick): keep the newest position. */
    if (ap->count > 0 && sample_back(ap, 0)->time_us == now_us) {
        ap->head = (uint8_t)((ap->head + UI_AUDIBLE_POSITION_SAMPLES - 1u) % UI_AUDIBLE_POSITION_SAMPLES);
        ap->count--;
    }
    ap->sample[ap->head].time_us = now_us;
    ap->sample[ap->head].position_ms = position_ms;
    ap->head = (uint8_t)((ap->head + 1u) % UI_AUDIBLE_POSITION_SAMPLES);
    if (ap->count < UI_AUDIBLE_POSITION_SAMPLES) {
        ap->count++;
    }

    if (latency_us == 0) {
        return position_ms;
    }
    uint64_t target_us = now_us > latency_us ? now_us - latency_us : 0;

    /* Newest sample at or before the target; s1 is the one after it. */
    uint8_t back = 0;
    while (back < ap->count && sample_back(ap, back)->time_us > target_us) {
        back++;
    }
    if (back == ap->count) {
        return sample_back(ap, (uint8_t)(ap->count - 1u))->position_ms;
    }
    const ui_audible_position_sample_t *s0 = sample_back(ap, back);
    if (back == 0) {
        return s0->position_ms;
    }
    const ui_audible_position_sample_t *s1 = sample_back(ap, (uint8_t)(back - 1u));

    uint64_t dt_us = s1->time_us - s0->time_us;
    uint32_t dp_ms = s1->position_ms >= s0->position_ms
                   ? s1->position_ms - s0->position_ms
                   : s0->position_ms - s1->position_ms;
    uint64_t limit_ms = dt_us * UI_AUDIBLE_POSITION_MAX_SPEED_PERMILLE / 1000000u +
                        UI_AUDIBLE_POSITION_STEP_SLACK_MS;
    if (dp_ms > limit_ms) {
        /* Seek, cue or loop wrap: not audible yet. */
        return s0->position_ms;
    }

    uint64_t part_ms = (uint64_t)dp_ms * (target_us - s0->time_us) / dt_us;
    return s1->position_ms >= s0->position_ms
         ? s0->position_ms + (uint32_t)part_ms
         : s0->position_ms - (uint32_t)part_ms;
}
