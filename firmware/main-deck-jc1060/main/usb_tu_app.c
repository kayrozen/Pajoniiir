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
#include "esp_wifi.h"
#include "esp_hosted.h"
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
static volatile uint32_t s_pb_cb_count;   /* completed playback transfers */
static volatile uint32_t s_pb_bytes;      /* playback bytes submitted */
static volatile uint32_t s_frame_bytes;   /* active frame size: 8 (S16) / 12 (S24_3) */

static void audio_try_start(void);
static void tuh_task_loop(void *arg);
static void audio_wifi_suspend(void);
static void audio_wifi_resume(void);

static uint8_t s_frame_scratch[192 * 12]; /* up to 192 frames of 4ch x 3B */

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
        audio_wifi_resume();
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

void tuh_audio_playback_cb(uint8_t idx, uint8_t stream_idx, uint16_t xferred_bytes)
{
    s_pb_cb_count++;
    s_pb_bytes += xferred_bytes;
}

void tuh_audio_event_cb(uint8_t idx, uint8_t stream_idx,
                        tuh_audio_event_t event, tusb_xfer_result_t result)
{
    ESP_LOGI(TU_TAG, "audio event: idx=%u stream=%u event=%d result=%d",
             idx, stream_idx, (int)event, (int)result);
    if (event == TUH_AUDIO_EVENT_START_COMPLETE && result != XFER_RESULT_SUCCESS) {
        s_audio_streaming = false;
    }
}

#endif /* CFG_TUH_AUDIO */

/* ------------------------------------------------------------------ */
/* Tone writer task: feeds the TinyUSB stream FIFO continuously        */
/* ------------------------------------------------------------------ */

#if CFG_TUH_AUDIO
/* Embedded WAV: 2 s, 4ch, 24-bit, 44.1 kHz sine (tone4ch.wav, EMBED_FILES).
 * Samples live in flash (memory-mapped) - the writer loop is a pure memcpy
 * from rodata, eliminating PSRAM latency from the audio path. */
extern const uint8_t _binary_tone4ch_wav_start[] asm("_binary_tone4ch_wav_start");
extern const uint8_t _binary_tone4ch_wav_end[]   asm("_binary_tone4ch_wav_end");

static const uint8_t *s_wav;      /* first sample frame */
static size_t   s_wav_frames;     /* total frames (12 B each, S24_3LE 4ch) */
static size_t   s_wav_pos;        /* read position, in frames */

static void tone_build_table(void)
{
    const size_t total = (size_t)(_binary_tone4ch_wav_end - _binary_tone4ch_wav_start);
    /* Find the data chunk: header is a canonical 44-byte PCM wave header. */
    size_t off = 12; /* skip RIFF/WAVE */
    while (off + 8 <= total) {
        uint32_t sz;
        memcpy(&sz, _binary_tone4ch_wav_start + off + 4, 4);
        if (memcmp(_binary_tone4ch_wav_start + off, "data", 4) == 0) {
            s_wav = _binary_tone4ch_wav_start + off + 8;
            s_wav_frames = sz / 12;
            s_wav_pos = 0;
            ESP_LOGI(TU_TAG, "wav: %u frames of 4ch S24", (unsigned)s_wav_frames);
            return;
        }
        off += 8 + sz + (sz & 1);
    }
    ESP_LOGE(TU_TAG, "wav: no data chunk");
}

/* Copy `frames` frames from the packed table into dst (fb bytes/frame).
 * fb==8 converts S24->S16 on the fly (cheap int16 pack, no sinf). */
static void tone_fill(uint8_t *dst, uint32_t frames, uint32_t fb)
{
    if (fb == 12) {
        size_t tail = s_wav_frames - s_wav_pos;
        if (frames <= tail) {
            memcpy(dst, s_wav + s_wav_pos * 12, frames * 12);
        } else {
            memcpy(dst, s_wav + s_wav_pos * 12, tail * 12);
            memcpy(dst + tail * 12, s_wav, (frames - tail) * 12);
        }
        s_wav_pos = (s_wav_pos + frames) % s_wav_frames;
    } else {
        for (uint32_t f = 0; f < frames; f++) {
            const uint8_t *q = s_wav + ((s_wav_pos + f) % s_wav_frames) * 12;
            int32_t v = (int32_t)q[0] | ((int32_t)q[1] << 8) | ((int32_t)q[2] << 16);
            int16_t s16 = (int16_t)(v >> 8);
            uint8_t *p = dst + f * 8;
            for (int c = 0; c < 4; c++) {
                p[c * 2 + 0] = (uint8_t)(s16 & 0xFF);
                p[c * 2 + 1] = (uint8_t)((s16 >> 8) & 0xFF);
            }
        }
        s_wav_pos = (s_wav_pos + frames) % s_wav_frames;
    }
}

static void tone_writer_task(void *arg)
{
    tone_build_table();
    if (s_wav == NULL) {
        ESP_LOGE(TU_TAG, "wav embed failed");
        vTaskDelete(NULL);
        return;
    }

    uint32_t stats_cb = 0;
    uint32_t stats_written = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (s_audio_idx == TUSB_INDEX_INVALID_8 || s_spk_stream == TUSB_INDEX_INVALID_8) {
            continue;
        }
        if (!s_audio_streaming) {
            audio_try_start();
            continue;
        }
        const uint32_t fb = s_frame_bytes;   /* 8 (S16) or 12 (S24_3), 4 ch */
        if (fb == 0) {
            continue;
        }
        uint32_t avail = tuh_audio_write_available(s_audio_idx, s_spk_stream);
        if (avail == 0) {
            /* FIFO full: driver is consuming - healthy. If the stream is not
             * actually active, log it once per second so we can see it. */
            if ((stats_cb % 1000) == 0) {
                ESP_LOGW(TU_TAG, "tone: avail=0, wrote_total=%lu pb_cbs=%lu",
                         (unsigned long)stats_written, (unsigned long)s_pb_cb_count);
            }
            stats_cb++;
            continue;
        }
        /* avail is already in whole frames (write_available divides by
         * frame_bytes) - do not divide again. */
        uint32_t frames = avail;
        if (frames == 0) {
            continue;
        }
        if (frames > 192) {
            frames = 192;
        }
        uint8_t *frame_buf = s_frame_scratch;
        tone_fill(frame_buf, frames, fb);
        uint32_t wrote = tuh_audio_write(s_audio_idx, s_spk_stream, frame_buf, frames);
        stats_written += wrote;
        stats_cb++;
        if ((stats_cb % 1000) == 0) {
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
    if (xTaskCreatePinnedToCore(tone_writer_task, "tone_wr", 6144, NULL,
                                24, NULL, 0) != pdPASS) {
        return ESP_FAIL;
    }
#endif

    /* tuh task: tuh_task() processes pending events and RETURNS - it must
     * be wrapped in an infinite loop (FreeRTOS aborts returning tasks).
     * Priority 24: ABOVE the Wi-Fi/ESP-Hosted tasks (~23) - during SDIO
     * bursts they otherwise preempt this task past the 1 ms iso deadline. */
    if (xTaskCreatePinnedToCore(tuh_task_loop, "tuh_task", 6144, NULL,
                                24, &s_tuh_task, 1) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void tuh_task_loop(void *arg)
{
    (void)arg;
    while (1) {
        tuh_task();
        taskYIELD();
    }
}

/* ------------------------------------------------------------------ */
/* Audio stream start once enumeration settles                         */
/* ------------------------------------------------------------------ */

/* Poll from the tone writer: when the DDJ stream is found and not started,
 * configure 44.1 kHz and start. */
#if CFG_TUH_AUDIO
/* Wi-Fi (ESP-Hosted over SDIO) bursts steal bus cycles from the USB iso
 * stream and cause audible glitches. Audio is king while playing: suspend
 * the radio for the duration of the stream, resume on unmount. */
static bool s_wifi_suspended;

static void audio_wifi_suspend(void)
{
    if (s_wifi_suspended) return;
    s_wifi_suspended = true;
    /* esp_wifi_stop() alone keeps the SDIO transport alive - the SDIO DMA
     * bursts keep stealing bus cycles from the USB iso stream. Tear the
     * whole ESP-Hosted transport down (same as the prod wifi_link). */
    esp_wifi_stop();
    esp_hosted_deinit();
    ESP_LOGW(TU_TAG, "Wi-Fi/SDIO transport suspended (audio active)");
}

static void audio_wifi_resume(void)
{
    if (!s_wifi_suspended) return;
    s_wifi_suspended = false;
    /* Full transport re-init, then hand the radio back to wifi_console. */
    esp_hosted_init();
    esp_wifi_start();
    esp_wifi_connect();
    ESP_LOGW(TU_TAG, "Wi-Fi/SDIO transport resumed (audio idle)");
}

bool usb_audio_wifi_suspended(void)
{
    return s_wifi_suspended;
}

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
        ESP_LOGI(TU_TAG, "config %u: %lu Hz %u ch fmt=%d", c,
                 (unsigned long)cfg.sample_rate, cfg.channels, (int)cfg.format);
        if (cfg.sample_rate == 44100 && cfg.channels == 4 &&
            cfg.format == TUH_AUDIO_FORMAT_S16_LE) {
            if (tuh_audio_configure(s_audio_idx, s_spk_stream, c)) {
                s_frame_bytes = 4u * tuh_audio_format_bytes(cfg.format);
                ESP_LOGI(TU_TAG, "configured 44.1 kHz 4ch fmt=%d, starting stream", (int)cfg.format);
                if (tuh_audio_start(s_audio_idx, s_spk_stream)) {
                    s_audio_streaming = true;
                    ESP_LOGI(TU_TAG, ">>> Audio streaming started (tone 440 Hz)");
                }
            }
        }
    }
}
#endif /* CFG_TUH_AUDIO */
