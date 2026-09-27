#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* v262: the engine position counts frames when they are mixed, the listener
 * hears them one output latency later (UAC ring + URBs + device). This keeps
 * the positions the UI sampled and returns the one from latency_us ago, i.e.
 * the audible position, as the anchor for ui_position_interpolator. Replaying
 * the sampled history covers pitch, jog bend, scratch, pause, cue and loop
 * wraps without knowing the speed; it only ever returns positions the engine
 * reported, so it never goes negative. */
#define UI_AUDIBLE_POSITION_SAMPLES 32u
/* Between two samples the position is interpolated unless it moved faster
 * than this (x1000, both ways): that is a seek, cue or loop wrap, held until
 * its sample is due. Scratch and jog stay well below. */
#define UI_AUDIBLE_POSITION_MAX_SPEED_PERMILLE 8000u
/* Engine positions move in output blocks (~5.3 ms) sampled by the UI clock. */
#define UI_AUDIBLE_POSITION_STEP_SLACK_MS 20u

typedef struct {
    uint64_t time_us;
    uint32_t position_ms;
} ui_audible_position_sample_t;

typedef struct {
    ui_audible_position_sample_t sample[UI_AUDIBLE_POSITION_SAMPLES];
    uint8_t head;                       /* next slot to write */
    uint8_t count;
} ui_audible_position_t;

void ui_audible_position_init(ui_audible_position_t *ap);

/* Records the engine position seen at now_us and returns the position it had
 * at now_us - latency_us. latency_us 0 returns position_ms. Before the history
 * reaches back far enough the oldest sample is returned. */
uint32_t ui_audible_position_update(ui_audible_position_t *ap,
                                    uint32_t position_ms,
                                    uint64_t now_us,
                                    uint32_t latency_us);

#ifdef __cplusplus
}
#endif
