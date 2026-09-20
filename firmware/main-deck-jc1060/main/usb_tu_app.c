/**
 * @file usb_tu_app.c
 * @brief DDJ-400 support on top of the TinyUSB host stack (UAC1 audio + MIDI).
 *
 * Replaces the IDF usb_host-based bring-up for USB. The P4 High-Speed UTMI
 * PHY is initialized through the IDF usb_phy API (as TinyUSB leaves PHY
 * handling to ESP-IDF on this target), then tuh runs on rhport 1 (OTG_HS).
 */
#include "usb_tu_app.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/usb_phy.h"   /* IDF6: public usb/usb_phy.h moved to esp_hw_support */
#include "hal/usb_utmi_hal.h"
#include "soc/usb_utmi_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#define TU_TAG "usb_tu"

/* rhport 1 = OTG_HS (UTMI) on ESP32-P4, see dwc2_esp32.h */
#define TU_HS_RHPORT 1

/* Tone: 440 Hz, S24_3LE interleaved, generated per packet by TinyUSB's
 * fractional scheduler (44.1 frames/ms exact). */
#define TONE_FREQ_HZ   440.0f

static usb_phy_handle_t s_phy;
static TaskHandle_t s_tuh_task;
static uint8_t s_audio_idx = TUSB_INDEX_INVALID_8;
static uint8_t s_spk_stream = TUSB_INDEX_INVALID_8;
static uint8_t s_midi_idx = TUSB_INDEX_INVALID_8;
static volatile bool s_audio_streaming;

static void audio_try_start(void);

static uint8_t s_frame_scratch[48 * 12]; /* up to 48 frames of 4ch x 3B */

/* ------------------------------------------------------------------ */
/* MIDI callbacks                                                      */
/* ------------------------------------------------------------------ */

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data)
{
    s_midi_idx = idx;
    ESP_LOGI(TU_TAG, "MIDI mounted: idx=%u rx_cables=%u", idx, mount_cb_data->rx_cable_count);
}

void tuh_midi_umount_cb(uint8_t idx)
{
    ESP_LOGI(TU_TAG, "MIDI unmounted: idx=%u", idx);
    if (s_midi_idx == idx) {
        s_midi_idx = TUSB_INDEX_INVALID_8;
    }
}

/* Log jog CC / note events, same semantics as the previous midi_host. */
static void midi_poll(void)
{
    if (s_midi_idx == TUSB_INDEX_INVALID_8) {
        return;
    }
    uint8_t cable;
    uint8_t pkt[4];
    while (tuh_midi_stream_read(s_midi_idx, &cable, pkt, sizeof(pkt)) > 0) {
        uint8_t status = pkt[0];
        if ((status & 0xF0) == 0xB0 && pkt[1] >= 24 && pkt[1] <= 25) {
            int32_t hi = pkt[2];
            ESP_LOGD(TU_TAG, "jog: cc=%u hi=%u", pkt[1], hi);
        } else if ((status & 0xF0) == 0x90 && pkt[2] > 0) {
            ESP_LOGI(TU_TAG, "note on: %u vel=%u", pkt[1], pkt[2]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Audio callbacks (only compiled with the audio class driver enabled) */
/* ------------------------------------------------------------------ */
#if CFG_TUH_AUDIO

void tuh_audio_mount_cb(uint8_t idx)
{
    ESP_LOGI(TU_TAG, "Audio mounted: idx=%u, streams=%u", idx,
             tuh_audio_stream_count(idx));

    s_audio_idx = idx;
    s_spk_stream = TUSB_INDEX_INVALID_8;
    for (uint8_t i = 0; i < tuh_audio_stream_count(idx); i++) {
        if (!tuh_audio_stream_exists(idx, i)) {
            continue;
        }
        if (tuh_audio_stream_direction(idx, i) == TUH_AUDIO_STREAM_PLAYBACK) {
            s_spk_stream = i;
            ESP_LOGI(TU_TAG, "playback stream found: %u, configs=%u",
                     i, tuh_audio_config_count(idx, i));
            break;
        }
    }
}

void tuh_audio_umount_cb(uint8_t idx)
{
    ESP_LOGI(TU_TAG, "Audio unmounted: idx=%u", idx);
    if (s_audio_idx == idx) {
        s_audio_idx = TUSB_INDEX_INVALID_8;
        s_spk_stream = TUSB_INDEX_INVALID_8;
        s_audio_streaming = false;
    }
}

void tuh_audio_mount_complete_cb(uint8_t idx)
{
    (void)idx;
}

bool tuh_audio_control_done_cb(uint8_t idx, uint8_t stream_idx,
                               tuh_audio_direction_t dir, uint8_t request,
                               uint8_t control_selector, uint16_t value,
                               uint8_t const *rx_buf, uint16_t len)
{
    ESP_LOGI(TU_TAG, "audio ctrl done: idx=%u req=0x%02X cs=0x%02X",
             idx, request, control_selector);
    return true;
}

void tuh_audio_stream_done_cb(uint8_t idx, uint8_t stream_idx,
                              tuh_audio_event_t event, uint32_t xferred_bytes)
{
    ESP_LOGI(TU_TAG, "stream event: idx=%u stream=%u event=%d bytes=%lu",
             idx, stream_idx, event, (unsigned long)xferred_bytes);
}

#endif /* CFG_TUH_AUDIO */

/* ------------------------------------------------------------------ */
/* Tone writer task: feeds the TinyUSB stream FIFO continuously        */
/* ------------------------------------------------------------------ */

#if CFG_TUH_AUDIO
static void tone_writer_task(void *arg)
{
    /* 1 s sine table at 44.1 kHz, amplitude -12 dBFS, 24-bit. */
    const size_t N = 44100;
    int32_t *tbl = heap_caps_malloc(N * sizeof(int32_t), MALLOC_CAP_8BIT);
    if (tbl == NULL) {
        ESP_LOGE(TU_TAG, "tone table alloc failed");
        vTaskDelete(NULL);
        return;
    }
    for (size_t i = 0; i < N; i++) {
        tbl[i] = (int32_t)(sinf(2.0f * (float)M_PI * TONE_FREQ_HZ
                                * (float)i / (float)N) * 0.25f * 8388607.0f);
    }

    size_t pos = 0;
    uint32_t stats_cb = 0;
    uint32_t stats_written = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2));
        if (s_audio_idx == TUSB_INDEX_INVALID_8 || s_spk_stream == TUSB_INDEX_INVALID_8) {
            continue;
        }
        if (!s_audio_streaming) {
            audio_try_start();
            continue;
        }
        uint32_t avail = tuh_audio_write_available(s_audio_idx, s_spk_stream);
        if (avail == 0) {
            continue;
        }
        /* Write whole 4ch 24-bit frames. Frame = 12 bytes: encode on the fly. */
        uint32_t frames = avail / 12;
        if (frames == 0) {
            continue;
        }
        if (frames > 48) {
            frames = 48;
        }
        uint8_t *frame_buf = s_frame_scratch;
        for (uint32_t f = 0; f < frames; f++) {
            int32_t v = tbl[pos];
            pos = (pos + 1) % N;
            uint8_t *p = frame_buf + f * 12;
            for (int c = 0; c < 4; c++) {
                p[c * 3 + 0] = (uint8_t)(v & 0xFF);
                p[c * 3 + 1] = (uint8_t)((v >> 8) & 0xFF);
                p[c * 3 + 2] = (uint8_t)((v >> 16) & 0xFF);
            }
        }
        uint32_t wrote = tuh_audio_write(s_audio_idx, s_spk_stream, frame_buf, frames);
        stats_written += wrote;
        stats_cb++;
        if ((stats_cb % 100) == 0) {
            ESP_LOGI(TU_TAG, "tone: wrote=%lu frames total", (unsigned long)stats_written);
        }
        midi_poll();
    }
}
#endif /* CFG_TUH_AUDIO */

/* ------------------------------------------------------------------ */
/* MIDI driver resolution: only keep the interface with MIDI streaming */
/* ------------------------------------------------------------------ */

tusb_desc_type_t dummy; /* avoid empty-translation-unit warnings */

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

esp_err_t usb_tu_start(void)
{
    /* PHY: UTMI High-Speed, host mode. TinyUSB leaves PHY init to ESP-IDF. */
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,          /* P4 OTG_HS = UTMI PHY (clocks the DWC2) */
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,   /* auto: FS or HS */
    };
    esp_err_t err = usb_new_phy(&phy_cfg, &s_phy);
    if (err != ESP_OK) {
        ESP_LOGE(TU_TAG, "usb_new_phy failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TU_TAG, "PHY ready");

    /* TinyUSB's dwc2_phy_init is empty on ESP32: bring up the UTMI PHY
     * clock/reset explicitly, otherwise the DWC2 registers are dead. */
    {
        int __DECLARE_RCC_ATOMIC_ENV = 0;
        (void)__DECLARE_RCC_ATOMIC_ENV;
        usb_utmi_hal_context_t utmi_ctx = { .dev = &USB_UTMI };
        usb_utmi_hal_init(&utmi_ctx);
        ESP_LOGI(TU_TAG, "UTMI PHY initialized");

    /* Use the High-Speed UTMI PHY on rhport 1 (default would fall back to
     * the embedded FS PHY and hang the core reset). */
    tuh_configure_param_t tuh_cfg = {
        .dwc2 = { .use_hs_phy = true },
    };
    tuh_configure(TU_HS_RHPORT, TUH_CFGID_DWC2, &tuh_cfg);
    }

    tusb_rhport_init_t host_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_AUTO,
    };
    if (!tuh_rhport_init(TU_HS_RHPORT, &host_init)) {
        ESP_LOGE(TU_TAG, "tuh_rhport_init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TU_TAG, "TinyUSB host started on HS rhport %d", TU_HS_RHPORT);

#if CFG_TUH_AUDIO
    if (xTaskCreate(tone_writer_task, "tone_wr", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
#endif

    /* tuh task */
    if (xTaskCreate((TaskFunction_t)tuh_task, "tuh_task", 4096, NULL, 5,
                    &s_tuh_task) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Audio stream start once enumeration settles                         */
/* ------------------------------------------------------------------ */

/* Poll from the tone writer: when the DDJ stream is found and not started,
 * configure 44.1 kHz and start. */
#if CFG_TUH_AUDIO
static void audio_try_start(void)
{
    if (s_audio_idx == TUSB_INDEX_INVALID_8 || s_spk_stream == TUSB_INDEX_INVALID_8 ||
        s_audio_streaming) {
        return;
    }
    uint8_t n_cfg = tuh_audio_config_count(s_audio_idx, s_spk_stream);
    for (uint8_t c = 0; c < n_cfg; c++) {
        tuh_audio_stream_config_t cfg;
        if (!tuh_audio_config_get(s_audio_idx, s_spk_stream, c, &cfg)) {
            continue;
        }
        ESP_LOGI(TU_TAG, "config %u: %lu Hz %u ch", c,
                 (unsigned long)cfg.sample_rate, cfg.channels);
        if (cfg.sample_rate == 44100 && cfg.channels == 4) {
            if (tuh_audio_configure(s_audio_idx, s_spk_stream, c)) {
                ESP_LOGI(TU_TAG, "configured 44.1 kHz 4ch, starting stream");
                if (tuh_audio_start(s_audio_idx, s_spk_stream)) {
                    s_audio_streaming = true;
                    ESP_LOGI(TU_TAG, ">>> Audio streaming started (tone 440 Hz)");
                }
            }
        }
    }
}
#endif /* CFG_TUH_AUDIO */
