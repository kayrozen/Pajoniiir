/**
 * @file ddj_tone.c
 * @brief DDJ-400 UAC bring-up tone: 440 Hz on MASTER, 880 Hz on PHONES.
 *
 * Feeds controller_usb_host_write_audio() with interleaved stereo blocks at
 * 44.1 kHz. The stream's clocked ring trims/duplicates to reconcile the
 * FreeRTOS tick pacing with the USB frame clock, so slight pacing error is
 * absorbed. When no UAC stream is up (no DDJ attached / still configuring)
 * the write returns ESP_ERR_INVALID_STATE and the task simply retries.
 */
#include "ddj_tone.h"

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "controller_usb_host.h"

static const char *TAG = "ddj_tone";

#define TONE_FRAMES_PER_BLOCK 192u   /* 4 ms at 48 kHz */
#define TONE_MASTER_FREQ_HZ   440.0f
#define TONE_PHONES_FREQ_HZ   880.0f
#define TONE_AMPLITUDE        6000   /* ~ -18 dBFS */
#define TONE_SILENCE          0      /* bisect: 1 = send digital silence */
#define TONE_ALL_SAME         1      /* bisect: 1 = same 440 Hz on all 4 channels */

static void ddj_tone_task(void *arg)
{
    static int16_t master[TONE_FRAMES_PER_BLOCK * 2];
    static int16_t phones[TONE_FRAMES_PER_BLOCK * 2];

    for (size_t i = 0; i < TONE_FRAMES_PER_BLOCK; ++i) {
#if TONE_SILENCE
        master[i * 2u] = 0;
        master[i * 2u + 1u] = 0;
        phones[i * 2u] = 0;
        phones[i * 2u + 1u] = 0;
#else
#if TONE_ALL_SAME
        master[i * 2u] = (int16_t)(sinf(2.0f * (float)M_PI * TONE_MASTER_FREQ_HZ *
                                        (float)i / 48000.0f) * TONE_AMPLITUDE);
        master[i * 2u + 1u] = master[i * 2u];
        phones[i * 2u] = master[i * 2u];
        phones[i * 2u + 1u] = master[i * 2u];
#else
        phones[i * 2u] = (int16_t)(sinf(2.0f * (float)M_PI * TONE_PHONES_FREQ_HZ *
                                        (float)i / 48000.0f) * TONE_AMPLITUDE);
        phones[i * 2u + 1u] = phones[i * 2u];
#endif
#endif
    }

    ESP_LOGW(TAG, "tone task running: 440 Hz master / 880 Hz phones @ 48000");
    TickType_t last_wake = xTaskGetTickCount();
    uint64_t accepted_total = 0;
    uint32_t rejected = 0;
    while (1) {
        const esp_err_t rc = controller_usb_host_write_audio(
            master, phones, TONE_FRAMES_PER_BLOCK, 48000u);
        if (rc == ESP_OK) {
            accepted_total += TONE_FRAMES_PER_BLOCK;
            if ((accepted_total / TONE_FRAMES_PER_BLOCK) % 2500u == 0u) {
                controller_usb_host_audio_stats_t stats;
                controller_usb_host_get_audio_stats(&stats);
                ESP_LOGW(TAG,
                         "tone: queued=%u submitted=%llu underrun=%llu dropped=%llu",
                         stats.ring_queued_frames,
                         (unsigned long long)stats.submitted_frames,
                         (unsigned long long)stats.underrun_frames,
                         (unsigned long long)stats.dropped_blocks);
            }
        } else if (rc == ESP_ERR_INVALID_STATE) {
            /* stream not up (no DDJ / configuring / stopped) */
            if (++rejected == 250u) {
                rejected = 0;
                ESP_LOGW(TAG, "tone: stream not accepting yet");
            }
        } else {
            ESP_LOGW(TAG, "tone write: %s", esp_err_to_name(rc));
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(4u));
    }
}

void ddj_tone_start(void)
{
    if (xTaskCreatePinnedToCore(ddj_tone_task, "ddj_tone", 4096, NULL,
                                4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "tone task create failed");
    }
}
