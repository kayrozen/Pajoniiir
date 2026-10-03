#pragma once
//
// v321: card-busy waits of the SD driver (sd_idle_wait.c), for the logs.
//
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t waits;          /* waits that found the card busy */
    uint32_t polls;          /* CMD13 sent by those waits */
    uint32_t max_wait_us;    /* longest wait */
    uint32_t timeouts;
} sd_idle_wait_stats_t;

/* Copies and clears the counters (any task). */
void sd_idle_wait_take_stats(sd_idle_wait_stats_t *out);

#ifdef __cplusplus
}
#endif
