/**
 * @file usb_audio_out.c
 * @brief Minimal UAC host OUT for the DDJ-400 audio streaming interface.
 *
 * The DDJ-400 exposes ifc 1 (audio streaming OUT) with two alt settings,
 * EP 0x01 isochronous, wMaxPacketSize 576 = 4 channels (master L/R +
 * phones L/R) x 48 kHz x 24-bit PCM at 1 ms (HS microframes).
 *
 * This module claims alt setting 1, issues SET_INTERFACE + SET_CUR
 * (48 kHz), then streams a continuous 440 Hz test tone on all channels
 * with a 2-transfer ping-pong. Deck audio routing replaces the tone next.
 */

#include "usb_audio_out.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define AUDIO_TAG "usb_audio"

#define AUDIO_IF_NUM        1
#define AUDIO_IF_ALT        2   /* alt2 = 4ch x 24-bit x 44.1 kHz (PulseAudio-verified path) */
#define AUDIO_EP_OUT        0x01
#define AUDIO_ISO_MPS       576
#define AUDIO_ISO_PACKETS   6               /* 6 x 1 ms packets per URB (kernel-like) */
#define AUDIO_N_URBS        6               /* deep queue: no gap on the iso pipe */
#define AUDIO_FRAMES_PER_PKT 45             /* ceil(44100/1000) = 45 (uac2_host calc_packet_size) */
#define AUDIO_CHANNELS      4               /* 1-2 master, 3-4 phones (Mixxx mapping) */
#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_FRAME_BYTES   (AUDIO_CHANNELS * 3) /* 24-bit samples, 3 bytes LE */
#define AUDIO_XFER_BYTES    (AUDIO_ISO_MPS * AUDIO_ISO_PACKETS) /* alloc cap */
#define TONE_FREQ_HZ        440.0f

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev_hdl;
    usb_transfer_t* xfer[AUDIO_N_URBS];
    uint8_t cur;
    bool running;
    bool claimed;
    int cb_count;
    int seq;
    size_t tbl_pos;
    int16_t* tone_tbl; /* 1 s of 44.1 kHz mono sine: 440 exact periods */
} audio_out_t;

static audio_out_t s_audio;

/* ------------------------------------------------------------------ */
/* Tone fill: 4ch x 24-bit PCM little-endian, packed                  */
/* ------------------------------------------------------------------ */

static void audio_fill_tone(usb_transfer_t* t)
{
    audio_out_t* a = &s_audio;
    /* Fill each 1 ms packet: 44.1 frames/ms -> 45 frames once every 10
     * packets, 44 otherwise. One continuous sample stream from a
     * precomputed table (no per-sample math in the callback). */
    uint8_t* buf = t->data_buffer;
    int idx = 0;
    /* 44 frames per 1 ms packet, constant - exactly what the Linux kernel
     * sends (verified via usbmon). The synchronous EP follows the SOF. */
    for (int p = 0; p < AUDIO_ISO_PACKETS; p++) {
        for (int f = 0; f < AUDIO_FRAMES_PER_PKT; f++) {
            int32_t v = a->tone_tbl[a->tbl_pos];
            a->tbl_pos = (a->tbl_pos + 1) % AUDIO_SAMPLE_RATE;
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                /* S24_3LE (verified via PulseAudio on a Linux host) */
                buf[idx++] = (uint8_t)(v & 0xFF);
                buf[idx++] = (uint8_t)((v >> 8) & 0xFF);
                buf[idx++] = (uint8_t)((v >> 16) & 0xFF);
            }
        }
        t->isoc_packet_desc[p].num_bytes = AUDIO_FRAMES_PER_PKT * AUDIO_FRAME_BYTES;
    }
    t->num_bytes = idx;
}

/* ------------------------------------------------------------------ */
/* Iso transfer callback                                              */
/* ------------------------------------------------------------------ */

static void audio_iso_out_cb(usb_transfer_t* t)
{
    audio_out_t* a = &s_audio;
    if (!a->running) {
        return;
    }
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(AUDIO_TAG, "iso OUT status=%d", t->status);
        a->running = false;
        return;
    }
    a->cb_count++;
    if ((a->cb_count % 500) == 0) { /* ~1 s */
        ESP_LOGI(AUDIO_TAG, "stats: cb=%d actual=%d pkt0.status=%d",
                 (int)a->cb_count, t->actual_num_bytes,
                 t->isoc_packet_desc[0].status);
    }
    audio_fill_tone(t); /* also sets each isoc_packet_desc num_bytes */
    esp_err_t res = usb_host_transfer_submit(t);
    if (res != ESP_OK) {
        ESP_LOGE(AUDIO_TAG, "resubmit failed: %s (0x%x)", esp_err_to_name(res), res);
        a->running = false;
    }
}

/* ------------------------------------------------------------------ */
/* Control transfers                                                  */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_ctrl_done;

static void ctrl_done_cb(usb_transfer_t* t)
{
    xSemaphoreGive(s_ctrl_done);
}

/* The enum task owns the client; while we wait for the control transfer
 * we must pump the client events ourselves (same task, sequential). */
static esp_err_t ctrl_transfer(usb_host_client_handle_t client,
                               usb_device_handle_t dev,
                               uint8_t bmRequestType, uint8_t bRequest,
                               uint16_t wValue, uint16_t wIndex,
                               uint16_t wLength, const uint8_t* data)
{
    usb_transfer_t* t = NULL;
    esp_err_t ret = usb_host_transfer_alloc(8 + wLength, 0, &t);
    if (ret != ESP_OK) {
        return ret;
    }
    t->data_buffer[0] = bmRequestType;
    t->data_buffer[1] = bRequest;
    t->data_buffer[2] = wValue & 0xFF;
    t->data_buffer[3] = wValue >> 8;
    t->data_buffer[4] = wIndex & 0xFF;
    t->data_buffer[5] = wIndex >> 8;
    t->data_buffer[6] = wLength & 0xFF;
    t->data_buffer[7] = wLength >> 8;
    if (wLength && data) {
        memcpy(&t->data_buffer[8], data, wLength);
    }
    t->num_bytes = 8 + wLength;
    t->device_handle = dev;
    t->callback = ctrl_done_cb;
    t->context = NULL;
    t->timeout_ms = 1000;

    xSemaphoreTake(s_ctrl_done, 0); /* drain stale token */
    ret = usb_host_transfer_submit_control(client, t);
    if (ret == ESP_OK) {
        for (int i = 0; i < 100; i++) {
            /* Pump the shared client so our callback can run. */
            usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
            if (xSemaphoreTake(s_ctrl_done, 0) == pdTRUE) {
                if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
                    ret = ESP_OK;
                } else {
                    ESP_LOGE(AUDIO_TAG, "ctrl 0x%02X status=%d", bRequest, t->status);
                    ret = ESP_FAIL;
                }
                break;
            }
        }
        if (ret == ESP_OK) {
            /* fallthrough */
        } else if (ret != ESP_FAIL) {
            ret = ESP_ERR_TIMEOUT;
        }
    }
    usb_host_transfer_free(t);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Attach / detach                                                    */
/* ------------------------------------------------------------------ */

bool usb_audio_out_attach(usb_host_client_handle_t client,
                          usb_device_handle_t dev_hdl,
                          const usb_config_desc_t* cfg_desc)
{
    if (s_audio.running) {
        ESP_LOGW(AUDIO_TAG, "already attached");
        return false;
    }
    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.client = client;
    s_audio.dev_hdl = dev_hdl;

    if (s_ctrl_done == NULL) {
        s_ctrl_done = xSemaphoreCreateBinary();
    }

    /* Precompute 1 s of sine (440 Hz at 44.1 kHz = exactly 440 periods,
     * so the table loops seamlessly). One-time cost, then the callback
     * only copies. */
    s_audio.tone_tbl = heap_caps_malloc(AUDIO_SAMPLE_RATE * sizeof(int16_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_audio.tone_tbl == NULL) {
        ESP_LOGE(AUDIO_TAG, "tone table alloc failed");
        return false;
    }
    for (int i = 0; i < AUDIO_SAMPLE_RATE; i++) {
        s_audio.tone_tbl[i] = (int16_t)(sinf(2.0f * (float)M_PI * TONE_FREQ_HZ
                                             * i / AUDIO_SAMPLE_RATE) * 0.25f
                                        * 32767.0f);
    }

    /* Verify ifc 1 alt 1 exists with the iso OUT EP. */
    bool found = false;
    int off = 0;
    const uint8_t* p = (const uint8_t*)cfg_desc;
    while (off + 2 <= cfg_desc->wTotalLength) {
        uint8_t len = p[off];
        uint8_t type = p[off + 1];
        if (len < 2 || off + len > cfg_desc->wTotalLength) {
            break;
        }
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* ifc = (const usb_intf_desc_t*)(p + off);
            if (ifc->bInterfaceNumber == AUDIO_IF_NUM
                && ifc->bAlternateSetting == AUDIO_IF_ALT
                && ifc->bNumEndpoints > 0) {
                found = true;
            }
        }
        off += len;
    }
    if (!found) {
        ESP_LOGW(AUDIO_TAG, "no audio streaming OUT alt %d", AUDIO_IF_ALT);
        return false;
    }

    esp_err_t ret = usb_host_interface_claim(client, dev_hdl, AUDIO_IF_NUM, AUDIO_IF_ALT);
    if (ret != ESP_OK) {
        ESP_LOGE(AUDIO_TAG, "claim ifc %d failed: %s", AUDIO_IF_NUM, esp_err_to_name(ret));
        return false;
    }
    s_audio.claimed = true;

    /* Activate the alt setting. */
    ret = ctrl_transfer(client, dev_hdl, 0x01, 0x0B /* SET_INTERFACE */,
                        AUDIO_IF_ALT, AUDIO_IF_NUM, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(AUDIO_TAG, "SET_INTERFACE failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* NOTE: no SET_CUR sampling-frequency request - the EP_GENERAL
     * descriptor (bmAttributes=0x00) shows this device does not support
     * frequency control; the rate is fixed at 44.1 kHz per alt setting.
     * The Linux kernel does not issue it either. */

    /* Deep-queued iso transfers (kernel-like): submit all, callback
     * refills and resubmits - the pipe never runs dry. */
    for (int i = 0; i < AUDIO_N_URBS; i++) {
        ret = usb_host_transfer_alloc(AUDIO_XFER_BYTES, AUDIO_ISO_PACKETS,
                                      &s_audio.xfer[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(AUDIO_TAG, "iso alloc failed");
            goto fail;
        }
        s_audio.xfer[i]->device_handle = dev_hdl;
        s_audio.xfer[i]->bEndpointAddress = AUDIO_EP_OUT;
        s_audio.xfer[i]->callback = audio_iso_out_cb;
        s_audio.xfer[i]->context = NULL;
        audio_fill_tone(s_audio.xfer[i]);
    }
    s_audio.running = true;
    for (int i = 0; i < AUDIO_N_URBS; i++) {
        ret = usb_host_transfer_submit(s_audio.xfer[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(AUDIO_TAG, "iso submit[%d] failed: %s", i, esp_err_to_name(ret));
            s_audio.running = false;
            goto fail;
        }
    }
    ESP_LOGI(AUDIO_TAG, "Audio OUT attached: ifc %d alt %d, EP 0x%02X, "
             "%dch x %d Hz x 24-bit - test tone playing",
             AUDIO_IF_NUM, AUDIO_IF_ALT, AUDIO_EP_OUT,
             AUDIO_CHANNELS, AUDIO_SAMPLE_RATE);
    return true;

fail:
    s_audio.running = false;
    for (int i = 0; i < AUDIO_N_URBS; i++) {
        if (s_audio.xfer[i]) {
            usb_host_transfer_free(s_audio.xfer[i]);
            s_audio.xfer[i] = NULL;
        }
    }
    if (s_audio.tone_tbl) {
        heap_caps_free(s_audio.tone_tbl);
        s_audio.tone_tbl = NULL;
    }
    if (s_audio.claimed) {
        usb_host_interface_release(client, dev_hdl, AUDIO_IF_NUM);
        s_audio.claimed = false;
    }
    return false;
}

void usb_audio_out_detach(usb_device_handle_t dev_hdl)
{
    if (s_audio.dev_hdl != dev_hdl) {
        return;
    }
    s_audio.running = false;
    for (int i = 0; i < AUDIO_N_URBS; i++) {
        if (s_audio.xfer[i]) {
            usb_host_transfer_free(s_audio.xfer[i]);
            s_audio.xfer[i] = NULL;
        }
    }
    if (s_audio.tone_tbl) {
        heap_caps_free(s_audio.tone_tbl);
        s_audio.tone_tbl = NULL;
    }
    if (s_audio.claimed) {
        usb_host_interface_release(s_audio.client, dev_hdl, AUDIO_IF_NUM);
        s_audio.claimed = false;
    }
    ESP_LOGI(AUDIO_TAG, "Audio OUT detached");
}
