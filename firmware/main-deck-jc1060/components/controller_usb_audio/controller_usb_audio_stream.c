/* SPDX-License-Identifier: Apache-2.0 */
#include "controller_usb_audio_stream.h"

#include <math.h>
#include <string.h>

#include "controller_audio_resampler.h"
#include "controller_audio_ring.h"
#include "esp_log.h"
#include "flx4_uac_descriptors.h"
#include "flx4_uac_packetizer.h"

/* v208: back to upstream 44.1 kHz. The DDJ-400 DAC runs at 44.1 kHz and its
 * observed alt 1/alt 2 descriptors list 44.1 kHz only
 * (docs/AUDIO_ENGINE_USB_SPEC.md D4/D6). With 48 kHz, select_stream_format()
 * found no candidate, the stream never started and every write was dropped. */
/* v222 test: 48 kHz. v205 listed 2 rates on alt 1 and the endpoint MPS 576 is
 * exactly 1 ms of 4ch S24_3LE at 48 kHz (48 x 12 B); the DDJ NAKs every
 * sampling-frequency request (v183, v219), so its clock cannot be read. If
 * it runs 48 kHz, 44/45-frame packets starve it ~4 frames per ms at the
 * right tempo. Keep AE_USB_SINK_RATE_HZ (audio_engine.c) equal. */
/* v223: v222 never streamed - the DDJ-400 descriptors announce only
 * "rates 1 [44100]", so the 48 kHz filter found no candidate (no "UAC ready").
 * Non-zero = force this rate: the announced-rate filter is bypassed (channels,
 * sample format and MPS are still checked, 24-bit alt still preferred) and the
 * packetizer paces STREAM_FORCE_RATE_HZ / 1000 frames per ms (576 B at 48 kHz).
 * 0 = honour the descriptor at 44.1 kHz. */
#define STREAM_FORCE_RATE_HZ 48000u
#if STREAM_FORCE_RATE_HZ
#define STREAM_RATE_HZ STREAM_FORCE_RATE_HZ
#else
#define STREAM_RATE_HZ 44100u
#endif
#define STREAM_CHANNELS 4u
#define STREAM_BYTES_PER_SAMPLE 2u
#define STREAM_RING_FRAMES 2048u
#define STREAM_TRANSFER_COUNT 3u
#define STREAM_PACKETS_PER_TRANSFER 4
/* Accepted engine output rates. The engine picks 44.1 or 48 kHz from the
 * track (audio_output_select_sample_rate); a 48 kHz source is resampled to the
 * 44.1 kHz stream here, exactly like upstream. */
#define STREAM_MIN_SOURCE_RATE_HZ 44100u
#define STREAM_MAX_SOURCE_RATE_HZ 48000u
/* Upstream FLX4 feeds the stream at -12 dB (>> 2, no rationale recorded).
 * v212: unity for the DDJ-400. The engine MAIN/cue blocks are already
 * limited to int16 full scale and the DDJ's MASTER/PHONES level knobs act
 * after the USB DAC, so the shift only cost 12 dB of level and 2 bits of the
 * 16-bit stream's resolution. */
#define STREAM_GAIN_SHIFT 0
/* v215 diagnostic: non-zero replaces the engine audio with a -6 dBFS sine of
 * this frequency on all four channels, generated at STREAM_RATE_HZ after the
 * resampler. A clean tone clears the ring/packing/USB path and points at the
 * engine content; a dirty one points at the transport. Off in normal builds. */
#define STREAM_TEST_TONE_HZ 0u

/* v218 (operator request): 24-bit subframe packing. 1 = upstream
 * left-justified int16 (s << 8, full-scale 24-bit). 0 = sign-extended int16
 * written unshifted into the 3-byte subframe (-1138 -> 8E FB FF), i.e. the
 * same waveform 48 dB below full scale. Flip back to 1 to restore upstream. */
#define STREAM_PCM24_LEFT_JUSTIFY 1

/* v219: 1 = query and program the endpoint sampling rate after SET_INTERFACE
 * (GET_CUR / SET_CUR / GET_CUR). 0 = upstream v208 behaviour (rate assumed).
 * A control step still pending after 1 s is logged from the HB stats.
 * v223: the stream is primed right after SET_INTERFACE and the rate requests
 * run afterwards as a diagnostic, so a NAKed request (v219 hung on the first
 * GET_CUR) no longer blocks playback. Order is SET_CUR (3) then GET_CUR (4);
 * the pre-SET_CUR GET_CUR is skipped because it NAKed forever in v219. */
#define STREAM_RATE_CONTROL 1

/* v221 diagnostic: bit n set = USB channel n carries audio, clear = digital
 * silence. Slot order MASTER L/R (0-1), PHONES L/R (2-3) matches the DDJ-400
 * channel names (ddj400_re 03_USB_SYSTEM, strings 4-7). 0x3 = MASTER only,
 * to hear what the device does with each pair. 0xF in normal builds. */
#define STREAM_CHANNEL_MASK 0xFu

/* v216: consumer-paced mode. Upstream never needs this - its engine blocks
 * on the PCM5102A I2S DMA, and the ring's clocked dup/trim only absorbs the
 * small I2S-vs-USB-SOF drift. The JC1060 has no local sink when MAIN is on
 * USB, and wall-clock pacing (185-187 blk/s) left a structural deficit
 * against the 44100 frame/s SOF drain: HIL v215 showed dup=2 per block and
 * under ~500 frames/s, i.e. repeated frames all the time - the distortion.
 * Instead the USB drain now clocks the engine exactly like the DMA did
 * upstream: controller_usb_audio_stream_pace_ready() tells the output task
 * to render the next block only once it fits under this ceiling, and while
 * the engine is paced this way the write skips dup/trim (there is no second
 * clock left to track). 1280 frames (5/8) caps latency at ~29 ms; with
 * 256-frame blocks and 176-frame (4-packet) drains the ring bottoms out
 * around 670 frames, ~15 ms of margin for render spikes. */
#define STREAM_PACE_CEILING_FRAMES ((STREAM_RING_FRAMES * 5u) / 8u)

/* Subframe slot of each stereo pair in the 4-channel USB frame, in the
 * DDJ-400 string-descriptor order: MASTER L/R (ch 1-2), PHONES L/R (ch 3-4).
 * v213 swapped them after HIL v212 heard MAIN on the phones; v214 reverts
 * that - the swap was a symptom of S16 frames sent to the 24-bit stream. */
#define STREAM_MASTER_SLOT 0u
#define STREAM_PHONES_SLOT 2u
#define RESAMPLE_INPUT_FRAMES 128u
/* Must cover controller_audio_resampler_output_bound() for the lowest source
 * rate: ceil(128 * STREAM_RATE_HZ / 44100) + 1 (129 at 44.1 kHz, 141 if the
 * stream ever runs at 48 kHz). */
#define RESAMPLE_OUTPUT_FRAMES \
    ((RESAMPLE_INPUT_FRAMES * STREAM_RATE_HZ + STREAM_MIN_SOURCE_RATE_HZ - 1u) / \
         STREAM_MIN_SOURCE_RATE_HZ + 1u)
/* Largest isochronous packet the packetizer can emit (1 ms at STREAM_RATE_HZ,
 * rounded up): 48 frames at 48 kHz, 45 at 44.1 kHz. */
#define STREAM_MAX_PACKET_FRAMES ((STREAM_RATE_HZ + 999u) / 1000u)

_Static_assert(RESAMPLE_OUTPUT_FRAMES >= 141u || STREAM_RATE_HZ != 48000u,
               "resample output buffer too small for 44.1 -> 48 kHz");

static const char *TAG = "controller_uac";

static void prime_and_ready(void);
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* v226: lowest ring level seen by pace_ready() since the last take, i.e. the
 * level just before each producer refill. Under s_mux. */
static uint32_t s_pace_low_water = UINT32_MAX;
static controller_audio_ring_t s_ring;
static int16_t s_ring_storage[STREAM_RING_FRAMES * STREAM_CHANNELS];
static controller_audio_resampler_t s_resampler;
static flx4_uac_packetizer_t s_packetizer;
static flx4_uac_playback_format_t s_format;
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_device;
static usb_transfer_t *s_control;
static usb_transfer_t *s_isoc[STREAM_TRANSFER_COUNT];
static bool s_isoc_active[STREAM_TRANSFER_COUNT];
static TaskHandle_t s_owner_task;
static UBaseType_t s_active_priority;
static UBaseType_t s_transition_priority;
static uint8_t s_control_step;
static bool s_control_active;
static TickType_t s_control_started;
static bool s_control_stall_logged;
static bool s_claimed;
static bool s_configuring;
static bool s_streaming;
static bool s_stopping;
static bool s_device_gone;
static bool s_flush_attempted;
static bool s_faulted;
/* Admission and in-flight ownership share one atomic word. Cleanup never waits
 * at USB priority for the lower-priority output owner; it polls on later turns. */
#define WRITE_ACCEPTING 1u
#define WRITE_ACTIVE 2u
static uint32_t s_write_gate;
/* Set by the first pace query of a stream; see STREAM_PACE_CEILING_FRAMES. */
static bool s_consumer_paced;
static void set_accepting(bool accepting)
{
    if (accepting) __atomic_fetch_or(&s_write_gate, WRITE_ACCEPTING, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&s_write_gate, ~WRITE_ACCEPTING, __ATOMIC_ACQ_REL);
}
static void finish_write(void)
{
    __atomic_fetch_and(&s_write_gate, ~WRITE_ACTIVE, __ATOMIC_RELEASE);
}
static uint64_t s_submitted_blocks;
static uint64_t s_dropped_blocks;
static uint64_t s_submitted_frames;
static uint32_t s_config_failures;
static uint32_t s_transfer_failures;
static uint32_t s_packet_failures;
static uint64_t s_packet_lost_frames;
/* v217: one-shot capture of the first non-silent 24-bit packet (16 bytes +
 * the int16 ring samples they came from), logged later from get_stats so the
 * isoc path never logs. */
#define STREAM_PACKET_DUMP_BYTES 16u
static uint8_t s_packet_dump[STREAM_PACKET_DUMP_BYTES];
static int16_t s_packet_dump_src[STREAM_PACKET_DUMP_BYTES / 3u];
static uint32_t s_packet_dump_state; /* 0 armed, 1 captured, 2 logged */
/* Monotonic per-boot identity for each successfully primed UAC stream. */
static uint32_t s_stream_epoch;

static void lower_to_transition_priority(void)
{
    if (s_owner_task && s_transition_priority > 0u) {
        vTaskPrioritySet(s_owner_task, s_transition_priority);
    }
}

static bool format_supports_rate(const flx4_uac_playback_format_t *format,
                                 uint32_t rate)
{
    if (!format) {
        return false;
    }
    if (format->sample_rate_continuous) {
        return format->sample_rate_count == 2u &&
               rate >= format->sample_rates[0] &&
               rate <= format->sample_rates[1];
    }
    for (uint8_t i = 0u; i < format->sample_rate_count; ++i) {
        if (format->sample_rates[i] == rate) {
            return true;
        }
    }
    return false;
}

static bool select_stream_format(const uint8_t *descriptor,
                                 size_t descriptor_length,
                                 flx4_uac_playback_format_t *out)
{
    flx4_uac_descriptor_result_t parsed;
    if (!flx4_uac_parse_playback_formats(descriptor, descriptor_length,
                                         &parsed)) {
        ESP_LOGW(TAG, "UAC format parse failed len=%u", (unsigned)descriptor_length);
        return false;
    }
    ESP_LOGW(TAG, "UAC formats parsed: %u", (unsigned)parsed.format_count);
    /* v214: every usable candidate is logged, then 24-bit wins over 16-bit.
     * The DDJ-400 streams 4ch 24-bit (ddj400_re 03_USB_SYSTEM, v205 cand 0:
     * bps 3, pkt 540 in MPS 576); feeding S16 to a device reading 3-byte
     * subframes shifts every frame - distortion and apparently swapped
     * MASTER/PHONES pairs. */
    int best = -1;
    bool best_pcm24 = false;
    for (uint8_t i = 0u; i < parsed.format_count; ++i) {
        const flx4_uac_playback_format_t *candidate = &parsed.formats[i];
        const bool pcm16 = candidate->bits_per_sample == 16u &&
                           candidate->bytes_per_sample == 2u;
        const bool pcm24 = candidate->bits_per_sample == 24u &&
                           candidate->bytes_per_sample == 3u;
        const uint32_t packet_bytes = STREAM_MAX_PACKET_FRAMES *
            STREAM_CHANNELS * candidate->bytes_per_sample;
        ESP_LOGW(TAG,
                 "cand %u: ifc %u alt %u ep 0x%02x mps %u ch %u bits %u bps %u "
                 "rates %u%s [%u %u %u] pcm16 %u pcm24 %u pkt %u rate_ok %u",
                 i, candidate->interface_num, candidate->alternate_setting,
                 candidate->endpoint_addr, candidate->max_packet_size,
                 candidate->channels, candidate->bits_per_sample,
                 candidate->bytes_per_sample,
                 (unsigned)candidate->sample_rate_count,
                 candidate->sample_rate_continuous ? " cont" : "",
                 (unsigned)candidate->sample_rates[0],
                 (unsigned)candidate->sample_rates[1],
                 (unsigned)candidate->sample_rates[2], pcm16, pcm24,
                 (unsigned)packet_bytes,
                 format_supports_rate(candidate, STREAM_RATE_HZ));
        if (candidate->channels == STREAM_CHANNELS &&
            (pcm16 || pcm24) &&
            candidate->max_packet_size >= packet_bytes &&
            (STREAM_FORCE_RATE_HZ ||
             format_supports_rate(candidate, STREAM_RATE_HZ)) &&
            (best < 0 || (pcm24 && !best_pcm24))) {
            best = i;
            best_pcm24 = pcm24;
        }
    }
    if (best < 0) {
        return false;
    }
    *out = parsed.formats[best];
    ESP_LOGW(TAG, "UAC selected cand %d (%u-bit, %u B/sample)", best,
             (unsigned)out->bits_per_sample, (unsigned)out->bytes_per_sample);
#if STREAM_FORCE_RATE_HZ
    if (!format_supports_rate(out, STREAM_RATE_HZ)) {
        ESP_LOGW(TAG, "UAC FORCED RATE %u Hz (descriptor announces %u Hz)",
                 (unsigned)STREAM_RATE_HZ, (unsigned)out->sample_rates[0]);
    }
#endif
    /* v217: an async OUT endpoint expects the host to follow its feedback;
     * this stream sends fixed 44/45-frame packets, so an async sink slips
     * samples at its own clock drift. */
    static const char *const sync_names[4] = { "none", "async", "adaptive",
                                               "sync" };
    ESP_LOGW(TAG, "UAC ep 0x%02x bmAttributes 0x%02x sync=%s "
                  "bSynchAddress 0x%02x in_ep 0x%02x",
             out->endpoint_addr, out->endpoint_attributes,
             sync_names[(out->endpoint_attributes >> 2) & 0x3u],
             out->sync_address, out->in_endpoint_addr);
    return true;
}

static int transfer_index(const usb_transfer_t *transfer)
{
    for (unsigned i = 0u; i < STREAM_TRANSFER_COUNT; ++i) {
        if (s_isoc[i] == transfer) {
            return (int)i;
        }
    }
    return -1;
}

static bool has_active_isoc(void)
{
    for (unsigned i = 0u; i < STREAM_TRANSFER_COUNT; ++i) {
        if (s_isoc_active[i]) {
            return true;
        }
    }
    return false;
}

static void mark_fault(bool configuration_failure)
{
    s_faulted = true;
    set_accepting(false);
    s_streaming = false;
    s_configuring = false;
    s_stopping = true;
    if (configuration_failure) {
        s_config_failures++;
    } else {
        s_transfer_failures++;
    }
    lower_to_transition_priority();
}

static esp_err_t prepare_and_submit(usb_transfer_t *transfer)
{
    const int index = transfer_index(transfer);
    if (index < 0 || !s_device || !s_claimed || s_stopping) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t offset = 0u;
    const uint8_t out_bps = s_format.bytes_per_sample;
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        const uint16_t frames = flx4_uac_packetizer_next_frames(&s_packetizer);
        const size_t bytes = (size_t)frames * STREAM_CHANNELS * out_bps;
        if (bytes > s_format.max_packet_size ||
            offset + bytes > transfer->data_buffer_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        portENTER_CRITICAL(&s_mux);
        if (out_bps == STREAM_BYTES_PER_SAMPLE) {
            (void)controller_audio_ring_read(
                &s_ring, (int16_t *)&transfer->data_buffer[offset], frames, true);
        } else {
            /* 24-bit device subframes: expand int16 ring samples to 3-byte
             * Type I PCM (left-justified, little-endian). */
            int16_t staged[STREAM_MAX_PACKET_FRAMES * STREAM_CHANNELS];
            if (frames > STREAM_MAX_PACKET_FRAMES) {
                portEXIT_CRITICAL(&s_mux);
                return ESP_ERR_INVALID_SIZE;
            }
            (void)controller_audio_ring_read(&s_ring, staged, frames, true);
            uint8_t *out8 = &transfer->data_buffer[offset];
            for (size_t s = 0; s < (size_t)frames * STREAM_CHANNELS; ++s) {
#if STREAM_PCM24_LEFT_JUSTIFY
                const uint32_t v = (uint32_t)(uint16_t)staged[s] << 8;
#else
                const int32_t v = (int32_t)staged[s];
#endif
                out8[s * 3u + 0u] = (uint8_t)(v & 0xffu);
                out8[s * 3u + 1u] = (uint8_t)((v >> 8) & 0xffu);
                out8[s * 3u + 2u] = (uint8_t)((v >> 16) & 0xffu);
            }
            if (__atomic_load_n(&s_packet_dump_state, __ATOMIC_RELAXED) == 0u &&
                frames >= 2u &&
                (staged[0] > 512 || staged[0] < -512)) {
                memcpy(s_packet_dump, out8, STREAM_PACKET_DUMP_BYTES);
                memcpy(s_packet_dump_src, staged, sizeof(s_packet_dump_src));
                __atomic_store_n(&s_packet_dump_state, 1u, __ATOMIC_RELEASE);
            }
        }
        portEXIT_CRITICAL(&s_mux);
        transfer->isoc_packet_desc[packet].num_bytes = (int)bytes;
        offset += bytes;
    }

    transfer->device_handle = s_device;
    transfer->bEndpointAddress = s_format.endpoint_addr;
    transfer->num_bytes = (int)offset;
    s_isoc_active[index] = true;
    const esp_err_t rc = usb_host_transfer_submit(transfer);
    if (rc != ESP_OK) {
        s_isoc_active[index] = false;
    }
    return rc;
}

static void isoc_callback(usb_transfer_t *transfer)
{
    const int index = transfer_index(transfer);
    if (index >= 0) {
        s_isoc_active[index] = false;
    }
    if (!transfer) {
        return;
    }
    if (transfer->status == USB_TRANSFER_STATUS_CANCELED ||
        transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        s_device_gone = s_device_gone ||
                        transfer->status == USB_TRANSFER_STATUS_NO_DEVICE;
        s_stopping = true;
        set_accepting(false);
        s_streaming = false;
        lower_to_transition_priority();
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "isochronous status=%d", (int)transfer->status);
        mark_fault(false);
        return;
    }
    /* HCD reports a completed URB even if individual ISO packets were skipped
     * or failed. Count loss before prepare_and_submit overwrites descriptors.
     * Isolated loss does not restart USB; terminal URB faults retain the bounded
     * recovery policy above. */
    for (int i = 0; i < transfer->num_isoc_packets; ++i) {
        const int wanted = transfer->isoc_packet_desc[i].num_bytes;
        const int actual = transfer->isoc_packet_desc[i].actual_num_bytes;
        const bool completed = transfer->isoc_packet_desc[i].status ==
                               USB_TRANSFER_STATUS_COMPLETED;
        if (!completed || actual != wanted) {
            __atomic_add_fetch(&s_packet_failures, 1u, __ATOMIC_RELAXED);
            const unsigned missing = !completed || actual < 0 || actual > wanted
                ? (unsigned)wanted : (unsigned)(wanted - actual);
            __atomic_add_fetch(&s_packet_lost_frames,
                (missing + STREAM_CHANNELS * s_format.bytes_per_sample - 1u) /
                    (STREAM_CHANNELS * s_format.bytes_per_sample), __ATOMIC_RELAXED);
        }
    }
    if (!s_stopping) {
        const esp_err_t rc = prepare_and_submit(transfer);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "isochronous resubmit: %s", esp_err_to_name(rc));
            mark_fault(false);
        }
    }
}

static esp_err_t submit_control_step(uint8_t step)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_control->data_buffer;
    memset(setup, 0, sizeof(*setup));
    if (step == 1u) {
        setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |
                               USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                               USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
        setup->bRequest = USB_B_REQUEST_SET_INTERFACE;
        setup->wValue = s_format.alternate_setting;
        setup->wIndex = s_format.interface_num;
        setup->wLength = 0u;
        s_control->num_bytes = sizeof(*setup);
    } else if (step == 2u || step == 4u) {
        /* v219: GET_CUR SAMPLING_FREQ_CONTROL on the isochronous endpoint,
         * before (2) and after (4) SET_CUR, so the log shows the rate the
         * device actually runs at. */
        setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
                               USB_BM_REQUEST_TYPE_TYPE_CLASS |
                               USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
        setup->bRequest = 0x81u;
        setup->wValue = 0x0100u;
        setup->wIndex = s_format.endpoint_addr;
        setup->wLength = 3u;
        memset(&s_control->data_buffer[sizeof(*setup)], 0, 3u);
        s_control->num_bytes = sizeof(*setup) + 3u;
    } else {
        setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |
                               USB_BM_REQUEST_TYPE_TYPE_CLASS |
                               USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
        setup->bRequest = 0x01u;
        setup->wValue = 0x0100u;
        /* UAC1 5.2.3.2: SAMPLING_FREQ_CONTROL targets the isochronous
         * endpoint (recipient ENDPOINT) - wIndex is the endpoint address
         * ALONE. A unit ID does not exist for an endpoint; putting the
         * interface in the high byte makes the request unmatchable and the
         * DDJ-400 NAKs it forever (observed on v183). */
        setup->wIndex = s_format.endpoint_addr;
        setup->wLength = 3u;
        uint8_t *rate = &s_control->data_buffer[sizeof(*setup)];
        rate[0] = (uint8_t)(STREAM_RATE_HZ & 0xffu);
        rate[1] = (uint8_t)((STREAM_RATE_HZ >> 8) & 0xffu);
        rate[2] = (uint8_t)((STREAM_RATE_HZ >> 16) & 0xffu);
        s_control->num_bytes = sizeof(*setup) + 3u;
    }
    s_control_step = step;
    s_control->device_handle = s_device;
    s_control->bEndpointAddress = 0u;
    s_control_started = xTaskGetTickCount();
    s_control_stall_logged = false;
    s_control_active = true;
    const esp_err_t rc = usb_host_transfer_submit_control(s_client, s_control);
    if (rc != ESP_OK) {
        s_control_active = false;
    }
    return rc;
}

static void control_callback(usb_transfer_t *transfer)
{
    s_control_active = false;
    if (!transfer) {
        return;
    }
    if (transfer->status == USB_TRANSFER_STATUS_CANCELED ||
        transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        s_device_gone = s_device_gone ||
                        transfer->status == USB_TRANSFER_STATUS_NO_DEVICE;
        s_stopping = true;
        set_accepting(false);
        s_configuring = false;
        lower_to_transition_priority();
        return;
    }
    if (s_stopping) {
        return;
    }
    const uint8_t step = s_control_step;
    const bool rate_step = step >= 2u;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED && !rate_step) {
        ESP_LOGW(TAG, "control step %u status=%d", step,
                 (int)transfer->status);
        mark_fault(true);
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        /* v219: a STALLed rate request means the endpoint has no
         * SAMPLING_FREQ_CONTROL; usbh re-arms EP0, so keep configuring. */
        ESP_LOGW(TAG, "UAC rate step %u status=%d (ignored)", step,
                 (int)transfer->status);
    } else if (step == 2u || step == 4u) {
        const uint8_t *r =
            &transfer->data_buffer[sizeof(usb_setup_packet_t)];
        const uint32_t rate = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
                              ((uint32_t)r[2] << 16);
        ESP_LOGW(TAG, "UAC GET_CUR rate %s SET_CUR: %u Hz (len %d)",
                 step == 2u ? "before" : "after", (unsigned)rate,
                 transfer->actual_num_bytes -
                     (int)sizeof(usb_setup_packet_t));
    } else if (step == 3u) {
        ESP_LOGW(TAG, "UAC SET_CUR %u Hz completed",
                 (unsigned)STREAM_RATE_HZ);
    }

    /* v219: the rate is no longer assumed. v214's "speed is right" did not
     * prove 44.1 kHz: on a synchronous endpoint the host bounds the data per
     * second, so a device clocking 48 kHz off SOF keeps the tempo but plays
     * each 44/45-frame packet 8.8% fast and starves ~3.9 frames per ms -
     * a 1 kHz buzz on top of everything.
     * v223 sequence: SET_INTERFACE (1), prime, then SET_CUR (3) and, only if
     * it completed, GET_CUR (4). Rate steps never fault the stream. */
    uint8_t next = 0u;
    if (step == 1u) {
        ESP_LOGW(TAG, "control step 1 complete, priming isochronous queue");
        prime_and_ready();
        if (!s_streaming || !STREAM_RATE_CONTROL) {
            return;
        }
        next = 3u;
    } else if (step == 3u &&
               transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        next = 4u;
    } else {
        ESP_LOGW(TAG, "UAC rate control sequence done (step %u)", step);
        return;
    }
    const esp_err_t rc = submit_control_step(next);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "UAC rate step %u submit: %s (ignored)", next,
                 esp_err_to_name(rc));
    }
}

static void prime_and_ready(void)
{
    if (s_streaming) {
        return;
    }
    bool all_submitted = true;
    for (unsigned i = 0u; i < STREAM_TRANSFER_COUNT; ++i) {
        const size_t bytes =
            (size_t)s_format.max_packet_size * STREAM_PACKETS_PER_TRANSFER;
        if (!s_isoc[i] && usb_host_transfer_alloc(
                bytes, STREAM_PACKETS_PER_TRANSFER, &s_isoc[i]) != ESP_OK) {
            all_submitted = false;
            break;
        }
        s_isoc[i]->callback = isoc_callback;
        if (prepare_and_submit(s_isoc[i]) != ESP_OK) {
            all_submitted = false;
            break;
        }
    }
    if (!all_submitted) {
        ESP_LOGW(TAG, "failed to prime UAC isochronous queue");
        mark_fault(true);
        return;
    }

    s_control_step = 0u;
    s_configuring = false;
    __atomic_add_fetch(&s_stream_epoch, 1u, __ATOMIC_RELEASE);
    s_streaming = true;
    set_accepting(true);
    if (s_owner_task && s_active_priority > 0u) {
        vTaskPrioritySet(s_owner_task, s_active_priority);
    }
    /* WARN level: CONFIG_LOG_DEFAULT_LEVEL=2 compiles out INFO. Neutral
     * label (v211): the path serves the DDJ-400 as well as the FLX4, and the
     * format fields come from the attached device's own descriptors. */
    ESP_LOGW(TAG,
             "UAC ready intf=%u alt=%u ep=0x%02X mps=%u %u Hz %uch/%u-bit "
             "(%u B/sample, pkt %u)",
             s_format.interface_num, s_format.alternate_setting,
             s_format.endpoint_addr, (unsigned)s_format.max_packet_size,
             (unsigned)STREAM_RATE_HZ, (unsigned)STREAM_CHANNELS,
             (unsigned)s_format.bits_per_sample,
             (unsigned)s_format.bytes_per_sample,
             (unsigned)(STREAM_MAX_PACKET_FRAMES * STREAM_CHANNELS *
                        s_format.bytes_per_sample));
#if STREAM_CHANNEL_MASK != 0xFu
    ESP_LOGW(TAG, "UAC CHANNEL MASK 0x%X (other channels silent)",
             (unsigned)STREAM_CHANNEL_MASK);
#endif
#if STREAM_TEST_TONE_HZ
    ESP_LOGW(TAG, "UAC TEST TONE %u Hz replaces engine audio",
             (unsigned)STREAM_TEST_TONE_HZ);
#endif
}

esp_err_t controller_usb_audio_stream_start(
    usb_host_client_handle_t client,
    usb_device_handle_t device,
    const uint8_t *config_descriptor,
    size_t config_descriptor_length,
    TaskHandle_t owner_task,
    UBaseType_t active_priority,
    UBaseType_t transition_priority)
{
    if (!client || !device || !config_descriptor ||
        config_descriptor_length < 9u || !owner_task ||
        active_priority == 0u || transition_priority == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!controller_usb_audio_stream_is_quiesced()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!select_stream_format(config_descriptor, config_descriptor_length,
                              &s_format)) {
        s_config_failures++;
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_client = client;
    s_device = device;
    s_owner_task = owner_task;
    s_active_priority = active_priority;
    s_transition_priority = transition_priority;
    s_device_gone = false;
    s_flush_attempted = false;
    s_faulted = false;
    s_stopping = false;
    s_configuring = true;
    s_streaming = false;
    set_accepting(false);
    __atomic_store_n(&s_consumer_paced, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_packet_dump_state, 0u, __ATOMIC_RELEASE);
    s_control_step = 0u;
    memset(s_isoc_active, 0, sizeof(s_isoc_active));
    memset(&s_resampler, 0, sizeof(s_resampler));
    flx4_uac_packetizer_init(&s_packetizer, STREAM_RATE_HZ,
                             STREAM_CHANNELS, s_format.bytes_per_sample);
    if (!s_ring.samples) {
        if (!controller_audio_ring_init(&s_ring, s_ring_storage,
                                        STREAM_RING_FRAMES, STREAM_CHANNELS,
                                        STREAM_RATE_HZ)) {
            mark_fault(true);
            return ESP_FAIL;
        }
    } else {
        portENTER_CRITICAL(&s_mux);
        controller_audio_ring_reset(&s_ring, STREAM_RATE_HZ);
        portEXIT_CRITICAL(&s_mux);
    }

    esp_err_t rc = usb_host_interface_claim(
        client, device, s_format.interface_num, s_format.alternate_setting);
    if (rc != ESP_OK) {
        s_config_failures++;
        s_configuring = false;
        s_client = NULL;
        s_device = NULL;
        return rc;
    }
    s_claimed = true;

    /* v219: room for a full EP0 MPS on the GET_CUR IN data stage. */
    rc = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64u, 0,
                                 &s_control);
    if (rc == ESP_OK) {
        s_control->callback = control_callback;
        rc = submit_control_step(1u);
    }
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "UAC configuration start: %s", esp_err_to_name(rc));
        mark_fault(true);
        return rc;
    }
    return ESP_OK;
}

void controller_usb_audio_stream_request_stop(bool device_gone)
{
    set_accepting(false);
    s_streaming = false;
    s_configuring = false;
    s_stopping = s_claimed || s_control || s_control_active;
    s_device_gone = s_device_gone || device_gone;
    lower_to_transition_priority();
}

bool controller_usb_audio_stream_poll_cleanup(void)
{
    if (!s_stopping) {
        return controller_usb_audio_stream_is_quiesced();
    }
    if (!s_device_gone && s_claimed && s_device && has_active_isoc() &&
        !s_flush_attempted) {
        s_flush_attempted = true;
        const esp_err_t halt_rc =
            usb_host_endpoint_halt(s_device, s_format.endpoint_addr);
        if (halt_rc == ESP_OK || halt_rc == ESP_ERR_INVALID_STATE) {
            const esp_err_t flush_rc =
                usb_host_endpoint_flush(s_device, s_format.endpoint_addr);
            if (flush_rc != ESP_OK && flush_rc != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "UAC endpoint flush: %s",
                         esp_err_to_name(flush_rc));
            }
        } else {
            ESP_LOGW(TAG, "UAC endpoint halt: %s", esp_err_to_name(halt_rc));
        }
    }
    if (s_control_active || has_active_isoc() ||
        (__atomic_load_n(&s_write_gate, __ATOMIC_ACQUIRE) & WRITE_ACTIVE)) {
        return false;
    }

    for (unsigned i = 0u; i < STREAM_TRANSFER_COUNT; ++i) {
        if (s_isoc[i]) {
            if (usb_host_transfer_free(s_isoc[i]) != ESP_OK) {
                return false;
            }
            s_isoc[i] = NULL;
        }
    }
    if (s_control) {
        if (usb_host_transfer_free(s_control) != ESP_OK) {
            return false;
        }
        s_control = NULL;
    }
    if (s_claimed) {
        const esp_err_t rc = usb_host_interface_release(
            s_client, s_device, s_format.interface_num);
        if (rc != ESP_OK) {
            return false;
        }
        s_claimed = false;
    }

    s_client = NULL;
    s_device = NULL;
    s_owner_task = NULL;
    s_control_step = 0u;
    s_configuring = false;
    s_streaming = false;
    s_stopping = false;
    s_device_gone = false;
    s_flush_attempted = false;
    memset(&s_format, 0, sizeof(s_format));
    memset(&s_resampler, 0, sizeof(s_resampler));
    portENTER_CRITICAL(&s_mux);
    controller_audio_ring_reset(&s_ring, STREAM_RATE_HZ);
    portEXIT_CRITICAL(&s_mux);
    return true;
}

uint32_t controller_usb_audio_stream_cleanup_blockers(void)
{
    uint32_t blockers = 0u;
    if (s_claimed) {
        blockers |= CONTROLLER_UAC_BLOCK_CLAIMED;
    }
    if (s_control) {
        blockers |= CONTROLLER_UAC_BLOCK_CONTROL;
    }
    if (s_control_active) {
        blockers |= CONTROLLER_UAC_BLOCK_CONTROL_ACTIVE;
    }
    if (has_active_isoc()) {
        blockers |= CONTROLLER_UAC_BLOCK_ISOC_ACTIVE;
    }
    if (__atomic_load_n(&s_write_gate, __ATOMIC_ACQUIRE) & WRITE_ACTIVE) {
        blockers |= CONTROLLER_UAC_BLOCK_WRITE_ACTIVE;
    }
    if (s_stopping) {
        blockers |= CONTROLLER_UAC_BLOCK_STOPPING;
    }
    if (s_device_gone) {
        blockers |= CONTROLLER_UAC_BLOCK_DEVICE_GONE;
    }
    return blockers;
}

void controller_usb_audio_stream_force_flush(void)
{
    /* poll_cleanup() skips halt/flush once the device is gone because usbh
     * already halts and flushes every endpoint of a gone device. If URBs are
     * still in flight after that, nothing else retires them and the device
     * can never be closed, so the root port is never re-armed. Halt on an
     * invalid port only marks the pipe halted; flush then retires pending
     * URBs with NO_DEVICE. */
    if (!s_stopping || !s_claimed || !s_device || !has_active_isoc()) {
        return;
    }
    s_flush_attempted = true;
    const esp_err_t halt_rc =
        usb_host_endpoint_halt(s_device, s_format.endpoint_addr);
    esp_err_t flush_rc = halt_rc;
    if (halt_rc == ESP_OK || halt_rc == ESP_ERR_INVALID_STATE) {
        flush_rc = usb_host_endpoint_flush(s_device, s_format.endpoint_addr);
    }
    ESP_LOGW(TAG, "UAC forced halt/flush ep 0x%02x: halt=%s flush=%s",
             s_format.endpoint_addr, esp_err_to_name(halt_rc),
             esp_err_to_name(flush_rc));
}

bool controller_usb_audio_stream_is_quiesced(void)
{
    return !s_claimed && !s_control && !s_control_active &&
           !has_active_isoc() &&
           __atomic_load_n(&s_write_gate, __ATOMIC_ACQUIRE) == 0u;
}

#if STREAM_TEST_TONE_HZ
static int16_t test_tone_next(void)
{
    static uint32_t phase;
    const float x = sinf(6.28318530718f * (float)phase / (float)STREAM_RATE_HZ);
    phase = (phase + STREAM_TEST_TONE_HZ) % STREAM_RATE_HZ;
    return (int16_t)(x * 16384.0f);
}
#endif

esp_err_t controller_usb_audio_stream_write(const int16_t *master_samples,
                                            const int16_t *headphone_samples,
                                            size_t frame_count,
                                            uint32_t source_sample_rate)
{
    if ((!master_samples && !headphone_samples) || frame_count == 0u ||
        source_sample_rate < STREAM_MIN_SOURCE_RATE_HZ ||
        source_sample_rate > STREAM_MAX_SOURCE_RATE_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t expected_gate = WRITE_ACCEPTING;
    if (!__atomic_compare_exchange_n(&s_write_gate, &expected_gate,
            WRITE_ACCEPTING | WRITE_ACTIVE, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_resampler.source_rate != source_sample_rate ||
        s_resampler.target_rate != STREAM_RATE_HZ ||
        s_resampler.channels != STREAM_CHANNELS) {
        if (!controller_audio_resampler_init(&s_resampler, source_sample_rate,
                                             STREAM_RATE_HZ,
                                             STREAM_CHANNELS)) {
            finish_write();
            return ESP_ERR_INVALID_ARG;
        }
    }

    int16_t input[RESAMPLE_INPUT_FRAMES * STREAM_CHANNELS];
    int16_t output[RESAMPLE_OUTPUT_FRAMES * STREAM_CHANNELS];
    bool dropped = false;
    while (frame_count > 0u) {
        const size_t chunk = frame_count > RESAMPLE_INPUT_FRAMES
                                 ? RESAMPLE_INPUT_FRAMES
                                 : frame_count;
        for (size_t i = 0u; i < chunk; ++i) {
            const int16_t ml = master_samples
                ? master_samples[i * 2u] >> STREAM_GAIN_SHIFT : 0;
            const int16_t mr = master_samples
                ? master_samples[i * 2u + 1u] >> STREAM_GAIN_SHIFT : 0;
            const int16_t hl = headphone_samples
                ? headphone_samples[i * 2u] >> STREAM_GAIN_SHIFT : ml;
            const int16_t hr = headphone_samples
                ? headphone_samples[i * 2u + 1u] >> STREAM_GAIN_SHIFT : mr;
            input[i * 4u + STREAM_MASTER_SLOT] = ml;
            input[i * 4u + STREAM_MASTER_SLOT + 1u] = mr;
            input[i * 4u + STREAM_PHONES_SLOT] = hl;
            input[i * 4u + STREAM_PHONES_SLOT + 1u] = hr;
        }
        const size_t output_frames = controller_audio_resampler_process(
            &s_resampler, input, chunk, output, RESAMPLE_OUTPUT_FRAMES);
#if STREAM_TEST_TONE_HZ
        for (size_t f = 0u; f < output_frames; ++f) {
            const int16_t tone = test_tone_next();
            for (unsigned c = 0u; c < STREAM_CHANNELS; ++c) {
                output[f * STREAM_CHANNELS + c] = tone;
            }
        }
#endif
#if STREAM_CHANNEL_MASK != 0xFu
        for (size_t f = 0u; f < output_frames; ++f) {
            for (unsigned c = 0u; c < STREAM_CHANNELS; ++c) {
                if ((STREAM_CHANNEL_MASK & (1u << c)) == 0u) {
                    output[f * STREAM_CHANNELS + c] = 0;
                }
            }
        }
#endif
        if (output_frames > 0u) {
            portENTER_CRITICAL(&s_mux);
            const uint64_t overrun_before = s_ring.overrun_frames;
            const uint32_t accepted =
                __atomic_load_n(&s_consumer_paced, __ATOMIC_ACQUIRE)
                    ? controller_audio_ring_write(&s_ring, output,
                                                  (uint32_t)output_frames)
                    : controller_audio_ring_write_clocked(
                          &s_ring, output, (uint32_t)output_frames);
            dropped = dropped || s_ring.overrun_frames != overrun_before;
            portEXIT_CRITICAL(&s_mux);
            __atomic_add_fetch(&s_submitted_frames, accepted,
                               __ATOMIC_RELAXED);
        }
        if (master_samples) {
            master_samples += chunk * 2u;
        }
        if (headphone_samples) {
            headphone_samples += chunk * 2u;
        }
        frame_count -= chunk;
    }
    if (dropped) {
        __atomic_add_fetch(&s_dropped_blocks, 1u, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&s_submitted_blocks, 1u, __ATOMIC_RELAXED);
    finish_write();
    return ESP_OK;
}

bool controller_usb_audio_stream_pace_ready(size_t frame_count,
                                           uint32_t source_sample_rate,
                                           bool *ready)
{
    if (!ready || source_sample_rate == 0u ||
        (__atomic_load_n(&s_write_gate, __ATOMIC_ACQUIRE) &
         WRITE_ACCEPTING) == 0u) {
        return false;
    }
    __atomic_store_n(&s_consumer_paced, true, __ATOMIC_RELEASE);
    const uint32_t incoming = (uint32_t)(
        ((uint64_t)frame_count * STREAM_RATE_HZ + source_sample_rate - 1u) /
        source_sample_rate);
    portENTER_CRITICAL(&s_mux);
    const uint32_t queued = s_ring.queued_frames;
    if (queued < s_pace_low_water) {
        s_pace_low_water = queued;
    }
    portEXIT_CRITICAL(&s_mux);
    *ready = queued + incoming <= STREAM_PACE_CEILING_FRAMES;
    return true;
}

uint32_t controller_usb_audio_stream_take_pace_low_water(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t low = s_pace_low_water;
    s_pace_low_water = UINT32_MAX;
    portEXIT_CRITICAL(&s_mux);
    return low;
}

void controller_usb_audio_stream_get_stats(
    controller_usb_audio_stream_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (s_control_active && !s_control_stall_logged &&
        (xTaskGetTickCount() - s_control_started) > pdMS_TO_TICKS(1000)) {
        s_control_stall_logged = true;
        ESP_LOGW(TAG, "UAC control step %u pending > 1 s (device NAK?); "
                      "set STREAM_RATE_CONTROL 0 if this is a rate step",
                 s_control_step);
    }
    if (__atomic_load_n(&s_packet_dump_state, __ATOMIC_ACQUIRE) == 1u) {
        const uint8_t *b = s_packet_dump;
        const int16_t *v = s_packet_dump_src;
        ESP_LOGW(TAG, "UAC pkt24 src %d %d %d %d %d -> "
                      "%02X %02X %02X %02X %02X %02X %02X %02X "
                      "%02X %02X %02X %02X %02X %02X %02X %02X",
                 v[0], v[1], v[2], v[3], v[4],
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        __atomic_store_n(&s_packet_dump_state, 2u, __ATOMIC_RELEASE);
    }
    out_stats->submitted_blocks =
        __atomic_load_n(&s_submitted_blocks, __ATOMIC_RELAXED);
    out_stats->dropped_blocks =
        __atomic_load_n(&s_dropped_blocks, __ATOMIC_RELAXED);
    out_stats->submitted_frames =
        __atomic_load_n(&s_submitted_frames, __ATOMIC_RELAXED);
    portENTER_CRITICAL(&s_mux);
    out_stats->ring_queued_frames = s_ring.queued_frames;
    out_stats->ring_capacity_frames = s_ring.frame_capacity;
    out_stats->ring_high_water_frames = s_ring.high_water_frames;
    out_stats->overrun_frames = s_ring.overrun_frames;
    out_stats->underrun_frames = s_ring.underrun_frames;
    out_stats->clock_trimmed_frames = s_ring.clock_trimmed_frames;
    out_stats->clock_duplicated_frames = s_ring.clock_duplicated_frames;
    portEXIT_CRITICAL(&s_mux);
    out_stats->config_failures = s_config_failures;
    out_stats->transfer_failures = s_transfer_failures;
    out_stats->packet_failures = __atomic_load_n(&s_packet_failures, __ATOMIC_RELAXED);
    out_stats->packet_lost_frames = __atomic_load_n(&s_packet_lost_frames, __ATOMIC_RELAXED);
    out_stats->stream_epoch = __atomic_load_n(&s_stream_epoch, __ATOMIC_ACQUIRE);
    out_stats->claimed = s_claimed;
    out_stats->configuring = s_configuring;
    out_stats->streaming = s_streaming;
    out_stats->faulted = s_faulted;
}
