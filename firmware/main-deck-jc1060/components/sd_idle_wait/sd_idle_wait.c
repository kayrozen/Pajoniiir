// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// Adapted for Pajoniiir (JC1060, v321): no frame trace; counters instead.

/**
 * @file sd_idle_wait.c
 * @brief Yielding replacement for IDF's sdmmc_wait_for_idle().
 *
 * After every SD write, and on a read that finds the card still busy, the IDF
 * driver waits for the card to leave its busy state by polling CMD13
 * back-to-back, with no yield during the first 100 ms
 * (components/sdmmc/sdmmc_common.c in ESP-IDF 6.0.2). A busy period is 1-45 ms
 * on consumer cards, so each wait turns into a storm of hundreds of host
 * commands. On the ESP32-P4 that storm slows CPU work on BOTH cores 3-50x
 * (esp-idf #19034, measured by p3a, https://github.com/fabkury/p3a,
 * docs/jitter/REPORT.md). On Pajoniiir (v314-v320 HIL) it stalled the audio
 * mix on CPU0 for 1.5-9 ms whenever a DJ Link download wrote to the card.
 *
 * Polling once per FreeRTOS tick removes the effect, at no measurable cost to
 * SD throughput (p3a: a 32 KB write still completes in ~5 ms). This is p3a's
 * variant (one CMD13 per tick, no busy-wait); Espressif's fix (release/v6.0
 * after v6.0.3, sdmmc_poll_delay_and_backoff) busy-waits up to ~1.5 ms first,
 * which here could be the deck loader on the audio core.
 *
 * Linked with --wrap=sdmmc_wait_for_idle (CMakeLists.txt); both callers are
 * in sdmmc_cmd.c, so every SD-mode wait goes through here. SPI hosts keep the
 * original.
 */

#include "sd_idle_wait.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_private/sdmmc_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t __real_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status);

static sd_idle_wait_stats_t s_stats;

static void sd_idle_wait_record(uint32_t polls, uint32_t wait_us, bool timeout)
{
    (void)__atomic_add_fetch(&s_stats.waits, 1u, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&s_stats.polls, polls, __ATOMIC_RELAXED);
    if (timeout) {
        (void)__atomic_add_fetch(&s_stats.timeouts, 1u, __ATOMIC_RELAXED);
    }
    uint32_t max = __atomic_load_n(&s_stats.max_wait_us, __ATOMIC_RELAXED);
    while (wait_us > max &&
           !__atomic_compare_exchange_n(&s_stats.max_wait_us, &max, wait_us, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

void sd_idle_wait_take_stats(sd_idle_wait_stats_t *out)
{
    if (!out) {
        return;
    }
    out->waits = __atomic_exchange_n(&s_stats.waits, 0u, __ATOMIC_RELAXED);
    out->polls = __atomic_exchange_n(&s_stats.polls, 0u, __ATOMIC_RELAXED);
    out->max_wait_us = __atomic_exchange_n(&s_stats.max_wait_us, 0u, __ATOMIC_RELAXED);
    out->timeouts = __atomic_exchange_n(&s_stats.timeouts, 0u, __ATOMIC_RELAXED);
}

esp_err_t __wrap_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status)
{
    if (host_is_spi(card)) {
        return __real_sdmmc_wait_for_idle(card, status);
    }
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    uint32_t polls = 0;
    while (!sdmmc_ready_for_data(status)) {
        if (esp_timer_get_time() - t0 > SDMMC_READY_FOR_DATA_TIMEOUT_US) {
            sd_idle_wait_record(polls, (uint32_t)(esp_timer_get_time() - t0), true);
            return ESP_ERR_TIMEOUT;
        }
        if (polls++ > 0) {
            vTaskDelay(1);   /* one CMD13 per tick instead of a poll storm */
        }
        err = sdmmc_send_cmd_send_status(card, &status);
        if (err != ESP_OK) {
            break;
        }
    }
    if (polls > 0) {
        sd_idle_wait_record(polls, (uint32_t)(esp_timer_get_time() - t0), false);
    }
    return err;
}
