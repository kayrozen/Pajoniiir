#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t submitted_blocks;
    uint64_t dropped_blocks;
    uint64_t submitted_frames;
    uint32_t ring_queued_frames;
    uint32_t ring_capacity_frames;
    uint32_t ring_high_water_frames;
    uint64_t overrun_frames;
    uint64_t underrun_frames;
    uint64_t clock_trimmed_frames;
    uint64_t clock_duplicated_frames;
    uint32_t config_failures;
    uint32_t transfer_failures;
    uint32_t packet_failures;
    uint64_t packet_lost_frames;
    uint32_t stream_epoch;
    /* v239: number of starts this boot, and the rc behind the last fault. */
    uint32_t start_seq;
    int32_t fault_rc;
    bool claimed;
    bool configuring;
    bool streaming;
    bool faulted;
} controller_usb_audio_stream_stats_t;

esp_err_t controller_usb_audio_stream_start(
    usb_host_client_handle_t client,
    usb_device_handle_t device,
    const uint8_t *config_descriptor,
    size_t config_descriptor_length,
    TaskHandle_t owner_task,
    UBaseType_t active_priority,
    UBaseType_t transition_priority);
void controller_usb_audio_stream_request_stop(bool device_gone);
bool controller_usb_audio_stream_poll_cleanup(void);
bool controller_usb_audio_stream_is_quiesced(void);
/* v217 recovery diagnostics. Bitmask of what keeps poll_cleanup() from
 * finishing (CONTROLLER_UAC_BLOCK_*), and a forced endpoint halt/flush for a
 * stop that stalls with isochronous transfers still in flight, including
 * after the device is gone. */
#define CONTROLLER_UAC_BLOCK_CLAIMED        (1u << 0)
#define CONTROLLER_UAC_BLOCK_CONTROL        (1u << 1)
#define CONTROLLER_UAC_BLOCK_CONTROL_ACTIVE (1u << 2)
#define CONTROLLER_UAC_BLOCK_ISOC_ACTIVE    (1u << 3)
#define CONTROLLER_UAC_BLOCK_WRITE_ACTIVE   (1u << 4)
#define CONTROLLER_UAC_BLOCK_STOPPING       (1u << 5)
#define CONTROLLER_UAC_BLOCK_DEVICE_GONE    (1u << 6)
uint32_t controller_usb_audio_stream_cleanup_blockers(void);
void controller_usb_audio_stream_force_flush(void);

esp_err_t controller_usb_audio_stream_write(const int16_t *master_samples,
                                            const int16_t *headphone_samples,
                                            size_t frame_count,
                                            uint32_t source_sample_rate);
/* v216 consumer pacing: returns false when the stream is not accepting audio
 * (caller keeps its own pacing). Otherwise *ready tells whether frame_count
 * source frames fit in the ring now; the first call switches the stream to
 * consumer-paced writes (no clocked dup/trim) until the next start. */
bool controller_usb_audio_stream_pace_ready(size_t frame_count,
                                           uint32_t source_sample_rate,
                                           bool *ready);
/* v226: lowest ring fill (frames) pace_ready() saw since the previous call,
 * then resets. UINT32_MAX = no pace_ready() call in between. */
uint32_t controller_usb_audio_stream_take_pace_low_water(void);
void controller_usb_audio_stream_get_stats(
    controller_usb_audio_stream_stats_t *out_stats);
/* v239: prints the start trace captured by the isoc callback (first
 * completed URB, first failed packet), once each per start, and only while
 * the stream is not running (UART output must not delay isoc resubmits).
 * Controller task only: the stats getters above are also read from audio/UI
 * tasks. */
void controller_usb_audio_stream_log_trace(void);

#ifdef __cplusplus
}
#endif

/* SPDX-License-Identifier: Apache-2.0 */
