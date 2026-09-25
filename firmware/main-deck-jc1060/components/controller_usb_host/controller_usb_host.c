/* SPDX-License-Identifier: Apache-2.0 */
#include "controller_usb_host.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "controller_usb_audio_stream.h"
#include "controller_usb_recovery_gate.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb_host_manager.h"

static const char *TAG = "controller_usb";
#define DEFAULT_TRANSFER_BYTES 64
#define FLX4_USB_VID 0x2B73u
/* Pioneer DJ VID 0x2B73. DDJ-FLX4 = PID 0x0045 (UAC1 16-bit, 4ch).
 * DDJ-400 = PID 0x0026 (UAC1 24-bit, 4ch) — per ddj400_re 03_USB_SYSTEM.
 * Both expose UAC1 + MIDI; UAC start is attempted for either and the
 * format selector picks what the device offers. */
#define FLX4_USB_PID 0x0045u
#define DDJ400_USB_PID 0x0026u
/* v210: UAC placement gate. Upstream starts UAC only for a direct child of
 * root port 1. The jc1060 fork reports parent.port_num as the zero-based
 * root index for root devices and the 1-based hub port for devices behind a
 * hub, and always leaves parent.dev_hdl NULL, so direct_root_child cannot
 * tell the two apart. Layout A puts the controller on the HS root (index 0,
 * see controller_bootstrap.c), which the upstream "== 1" test rejected
 * silently: no descriptor parse, no claim, every write INVALID_STATE. Port 1
 * (FS root, or hub port 1) is kept because UAC was proven there on v205/206.
 * The device is identified by VID/PID and the format selector still
 * validates the stream. */
#define CONTROLLER_UAC_ROOT_PORT 0u
#define CONTROLLER_UAC_LEGACY_PORT 1u
/* Keep the UAC consumer level with the priority-6 ae_output producer. Raising
 * this client above ae_output lets dense jog MIDI traffic continuously preempt
 * the only task that refills the UAC ring, producing headphone underruns while
 * both deck PCM rings remain healthy. Equal priority retains prompt ISO event
 * service through FreeRTOS time slicing and ae_output's explicit yield/block
 * points without increasing the audio ring or its cue latency. */
#define CONTROLLER_USB_ACTIVE_PRIORITY 6u
/* v217 unplug/replug recovery. The root port is re-armed by the Host Library
 * only after every client has closed the gone device, so a close that stalls
 * leaves the controller root dead (no NEW_DEV, no MIDI, no UAC). After
 * CLOSE_FORCE_MS the endpoints are halted/flushed even for a gone device;
 * progress is logged every CLOSE_LOG_MS. */
#define CONTROLLER_CLOSE_LOG_MS 1000u
#define CONTROLLER_CLOSE_FORCE_MS 1000u
/* v233: esp-usb handle_ep0_dequeue() (usbh.c) runs the UAC control transfer
 * callback - unblocking this priority-6 task on the same core as the
 * priority-4 usb_hostd daemon - before it decrements
 * num_ctrl_xfers_inflight. Closing the device in that window trips
 * assert(num_ctrl_xfers_inflight == 0) in usbh_dev_close() and aborts
 * (unplug / "Dev 1 EP 0 Error" with UAC step 4 still pending). Wait this
 * long after UAC cleanup completes, blocked in the client loop, so the
 * daemon finishes the dequeue before usb_host_device_close(). */
#define CONTROLLER_CLOSE_EP0_SETTLE_MS 20u
/* v237: root recovery after a fault or unplug close, the upstream
 * usb_storage pattern: power the root off, wait SETTLE_MS, power it on, and
 * repeat every RETRY_MS (SLOW_MS after MAX_CYCLES) until a device probes on
 * that root. v236 sent one manager recovery request instead; the manager
 * only powers off a disconnected root, so a faulted controller that stayed
 * enumerated was "suppressed" and never produced a NEW_DEV again. The
 * forced power-off is a supported hub path (NOT_POWERED + disconnect =
 * device gone). POWER_ON returns INVALID_STATE until the daemon has handled
 * that disconnect, so it is retried like the manager does. */
#define CONTROLLER_ROOT_CYCLE_SETTLE_MS 150u
#define CONTROLLER_ROOT_CYCLE_RETRY_MS 900u
#define CONTROLLER_ROOT_CYCLE_MAX_CYCLES 8u
#define CONTROLLER_ROOT_CYCLE_SLOW_MS 30000u
#define CONTROLLER_ROOT_POWER_ON_RETRY_MS 20u
#define CONTROLLER_ROOT_POWER_ON_TIMEOUT_MS 1000u
/* v238: on this fork a forced root power-off produces no connection event
 * afterwards (HIL v237: POWER_ON INVALID_STATE from cycle #5, no NEW_DEV
 * after the replug). After HOST_RESTART_AFTER cycles without a probe on the
 * root, the whole Host Library is uninstalled and reinstalled like at boot
 * (usb_host_manager_request_host_restart), the only path proven to
 * re-enumerate here. It also remounts the USB storage (~5 s). At most
 * HOST_RESTART_MAX per recovery episode; the power-cycles then go on alone.
 * SETTLE_MS leaves the reinstalled stack time to enumerate before the next
 * power-cycle. */
#define CONTROLLER_HOST_RESTART_AFTER_CYCLES 3u
#define CONTROLLER_HOST_RESTART_MAX 3u
#define CONTROLLER_HOST_RESTART_SETTLE_MS 5000u
#define CONTROLLER_HOST_RESTART_WAIT_MS 30000u
#define CONTROLLER_REGISTER_RETRY_MS 1000u
/* v239: escalation off. HIL v238: the host restart unmounts the USB
 * storage and the library does not come back, so the track in play is lost;
 * the operator rejected it. The code stays; 1 re-enables it. */
#define CONTROLLER_HOST_RESTART_ENABLED 0
/* v239: forced root power-cycle off as well. HIL v237: after a forced
 * power-off this fork's root reports no connection any more, and the loop,
 * armed 900 ms after an unplug, killed the root before the replug. An
 * unplugged controller comes back with its own NEW_DEV (v234-v236). A fault
 * close with the device still enumerated re-probes its address instead
 * (v234 path), with FAULT_REPROBE backoff over consecutive faults. */
#define CONTROLLER_ROOT_CYCLE_ENABLED 0
/* v239: a UAC fault no longer closes the controller: the stream alone is
 * stopped and restarted (MIDI stays up). The first restart delay doubles
 * with each fault less than FAULT_STREAK_RESET_MS after the previous one. */
#define CONTROLLER_FAULT_STREAK_RESET_MS 30000u
#define CONTROLLER_ROOT_UNKNOWN 0xFFu
/* v234: a probe that fails after enumeration (replug: MIDI EP alloc
 * ESP_ERR_NO_MEM at interface claim) closes our handle but leaves the device
 * enumerated on its root. No NEW_DEV follows, and the manager refuses to
 * power-cycle a root with a connected device, so re-arming cannot help. The
 * same address is re-probed with a doubling backoff instead, until it
 * succeeds or the device is gone (open fails). */
#define CONTROLLER_REPROBE_FIRST_MS 250u
#define CONTROLLER_REPROBE_MAX_MS 5000u
#define CONTROLLER_REPROBE_LOG_EVERY 12u
/* v236: internal DMA reserve for the controller's endpoint resources. The
 * claims allocate 512-aligned qTD lists (INTR 256 B, ISOC 512 B; each needs
 * ~0.8-1 KB contiguous) and the URB buffers (3 x 2304 B UAC isoc, MIDI/EP0
 * small ones), all from MALLOC_CAP_DMA | INTERNAL. After an unplug that
 * heap was fragmented (v235: DMA free=23779 largest=2304), so every replug
 * claim failed with ESP_ERR_NO_MEM. Chunks are taken once at boot, freed
 * just before the MIDI interface claim so the Host Library allocations land
 * in them, and re-taken when the controller has closed (its buffers then
 * coalesce back into the same holes). Each chunk fits one isoc URB or two
 * worst-case aligned qTD lists; six cover the ~12 KB the DDJ-400 holds
 * (4 INTR + 2 ISOC lists, 3 isoc + 3 small URBs). While the controller is
 * connected the chunks are its own allocations, so the steady-state
 * footprint is unchanged; unplugged, the reserve holds ~15 KB.
 * v237: v236 kept the reserve released for the whole session, and the
 * leftover of the released chunks was taken by small internal allocations
 * (SPIRAM_MALLOC_ALWAYSINTERNAL) before the UAC prime allocated its 3 x
 * 2304 B isoc URBs after SET_INTERFACE: replug prime NO_MEM, UAC fault. The
 * isoc URBs are now allocated by the UAC start, inside the probe burst, and
 * the unused part of the released reserve is re-taken right after the burst
 * (dma_reserve_settle); a UAC start retry releases what is held again. */
#define CONTROLLER_DMA_RESERVE_CHUNKS 6u
#define CONTROLLER_DMA_RESERVE_CHUNK_BYTES 2560u
#define CONTROLLER_DMA_RESERVE_TOPUP_MS 1000u
#define CONTROLLER_DMA_CAPS (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
/* v240: HIL v239 still hit NO_MEM on the replug isoc URB alloc with the
 * reserve in place. With CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM the fork
 * allocates URB data buffers and qTD lists in PSRAM (only the per-port frame
 * list, allocated at install, stays internal), so the reserve has nothing
 * left to protect and is not taken. */
#if defined(CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM)
#define CONTROLLER_DMA_RESERVE_ENABLED 0
#else
#define CONTROLLER_DMA_RESERVE_ENABLED 1
#endif
/* v235: same for the UAC start. A replug re-attached MIDI but the UAC
 * interface claim hit ESP_ERR_NO_MEM once and was never retried, so audio
 * stayed dead while MIDI worked. The start is retried with the re-probe
 * backoff until it succeeds or the controller closes (device gone). */

typedef struct {
    uint32_t generation;
    uint8_t packet[4];
} controller_midi_out_item_t;

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    usb_transfer_t *in_transfer;
    usb_transfer_t *out_transfer;
    QueueHandle_t out_queue;
    QueueHandle_t probe_queue;
    controller_usb_host_config_t config;
    controller_usb_identity_t identity;
    bool opened;
    bool claimed;
    bool in_active;
    bool out_active;
    bool closing;
    bool device_gone;
    bool out_generation_closed;
    bool midi_flush_attempted;
    controller_usb_recovery_gate_t recovery_gate;
    /* v217: survives the identity memset at the end of close_step(). */
    uint8_t recovery_root;
    bool close_timing;
    bool close_forced;
    TickType_t close_started;
    TickType_t close_last_log;
    /* v233: when UAC cleanup (the only EP0 user) first reported done. */
    bool close_uac_done;
    TickType_t close_uac_done_at;
    /* v236: DMA free when the close began, to log what the controller held. */
    size_t close_dma_free;
    bool awaiting_reattach;
    TickType_t detached_at;
    /* v237: root power-cycle loop, until a device probes on that root. */
    bool root_cycle_pending;
    uint8_t root_cycle_root;
    uint32_t root_cycle_count;
    TickType_t root_cycle_at;
    /* v238: host restarts requested in this recovery episode. */
    uint32_t host_restart_count;
    /* v239: address to re-probe after a fault close (0 = none), and the
     * consecutive fault streaks of the controller and of the UAC stream. */
    uint8_t fault_address;
    uint32_t fault_streak;
    TickType_t fault_last_at;
    uint32_t uac_fault_streak;
    TickType_t uac_fault_last_at;
    /* v237: root of the device being probed, UNKNOWN until its info read. */
    uint8_t probe_root;
    /* v234: re-probe of an enumerated device whose probe failed. */
    bool reprobe_pending;
    uint8_t reprobe_address;
    uint32_t reprobe_count;
    uint32_t reprobe_delay_ms;
    TickType_t reprobe_at;
    /* v235: UAC start retry while MIDI stays up. */
    bool uac_retry_pending;
    uint32_t uac_retry_count;
    uint32_t uac_retry_delay_ms;
    TickType_t uac_retry_at;
} controller_state_t;

static controller_state_t s_state;
static esp_err_t s_register_result = ESP_ERR_INVALID_STATE;
static bool s_registered;
static bool s_connected;
static bool s_accepting_out;
static uint32_t s_devices_probed;
static uint32_t s_descriptor_rejects;
static uint32_t s_midi_descriptor_rejects;
static uint32_t s_interface_claim_failures;
static uint32_t s_transfer_alloc_failures;
static uint32_t s_midi_connects;
static uint32_t s_midi_disconnects;
static uint32_t s_midi_packets;
static uint32_t s_midi_bytes;
static uint32_t s_midi_parse_rejects;
static uint32_t s_midi_in_submit_failures;
static uint32_t s_midi_out_submit_failures;
static uint32_t s_midi_out_queue_drops;
/* v220 diagnostic: the first MIDI OUT transfers of each connection are
 * logged at submit and completion, so a HIL log shows whether LED feedback
 * really reaches the OUT endpoint and whether the device accepts it. */
#define MIDI_OUT_LOG_LIMIT 16u
static uint32_t s_midi_out_submit_logged;
static uint32_t s_midi_out_done_logged;
static uint32_t s_probe_event_drops;
static uint32_t s_recovery_requests;
static int32_t s_last_probe_result = ESP_ERR_INVALID_STATE;
static uint16_t s_last_seen_vid;
static uint16_t s_last_seen_pid;
static uint16_t s_last_config_total_length;
static uint8_t s_last_probe_stage;
static uint8_t s_last_probe_address;
static uint8_t s_last_parent_port;
static bool s_last_direct_root;
static uint32_t s_out_generation = 1u;
static void *s_dma_reserve[CONTROLLER_DMA_RESERVE_CHUNKS];
static TickType_t s_dma_reserve_last_fill;
/* v237: bytes freed by the last release and DMA free right after it, so
 * dma_reserve_settle() knows what the burst consumed. 0 = no open burst. */
static size_t s_dma_burst_released;
static size_t s_dma_burst_free;
#if CONTROLLER_HOST_RESTART_ENABLED
/* v238: usb_host_manager restart participant id, 0 if not registered. */
static uint32_t s_restart_participant;
#endif

static inline void count_inc(uint32_t *value)
{
    (void)__atomic_add_fetch(value, 1u, __ATOMIC_RELAXED);
}

static inline uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)ticks * (uint32_t)portTICK_PERIOD_MS;
}

static unsigned dma_reserve_count(void)
{
    unsigned count = 0u;
    for (size_t i = 0u; i < CONTROLLER_DMA_RESERVE_CHUNKS; ++i) {
        count += s_dma_reserve[i] ? 1u : 0u;
    }
    return count;
}

/* Controller task (or init before the task exists) only. */
static void dma_reserve_fill(const char *why)
{
    if (!CONTROLLER_DMA_RESERVE_ENABLED) {
        if (why && strcmp(why, "boot") == 0) {
            ESP_LOGW(TAG, "USB DMA reserve off: USB-DWC buffers in PSRAM "
                          "(internal DMA free=%u largest=%u)",
                     (unsigned)heap_caps_get_free_size(CONTROLLER_DMA_CAPS),
                     (unsigned)heap_caps_get_largest_free_block(
                         CONTROLLER_DMA_CAPS));
        }
        return;
    }
    const unsigned before = dma_reserve_count();
    s_dma_reserve_last_fill = xTaskGetTickCount();
    s_dma_burst_released = 0u;
    for (size_t i = 0u; i < CONTROLLER_DMA_RESERVE_CHUNKS; ++i) {
        if (!s_dma_reserve[i]) {
            s_dma_reserve[i] = heap_caps_malloc(
                CONTROLLER_DMA_RESERVE_CHUNK_BYTES, CONTROLLER_DMA_CAPS);
        }
    }
    const unsigned after = dma_reserve_count();
    if (after != before || after < CONTROLLER_DMA_RESERVE_CHUNKS) {
        if (after == before && why == NULL) {
            return; /* periodic top-up that changed nothing: stay quiet */
        }
        ESP_LOGW(TAG, "USB DMA reserve %u/%u chunks of %u B (%s; DMA free=%u "
                      "largest=%u)",
                 after, (unsigned)CONTROLLER_DMA_RESERVE_CHUNKS,
                 (unsigned)CONTROLLER_DMA_RESERVE_CHUNK_BYTES,
                 why ? why : "top-up",
                 (unsigned)heap_caps_get_free_size(CONTROLLER_DMA_CAPS),
                 (unsigned)heap_caps_get_largest_free_block(
                     CONTROLLER_DMA_CAPS));
    }
}

/* Opens an allocation burst; dma_reserve_settle() closes it. */
static void dma_reserve_release(const char *why)
{
    const unsigned count = dma_reserve_count();
    s_dma_burst_released = 0u;
    if (!CONTROLLER_DMA_RESERVE_ENABLED || count == 0u) {
        return;
    }
    for (size_t i = 0u; i < CONTROLLER_DMA_RESERVE_CHUNKS; ++i) {
        heap_caps_free(s_dma_reserve[i]);
        s_dma_reserve[i] = NULL;
    }
    s_dma_burst_released = (size_t)count * CONTROLLER_DMA_RESERVE_CHUNK_BYTES;
    s_dma_burst_free = heap_caps_get_free_size(CONTROLLER_DMA_CAPS);
    ESP_LOGW(TAG, "USB DMA reserve: %u chunks released for the %s "
                  "(DMA free=%u largest=%u)",
             count, why, (unsigned)s_dma_burst_free,
             (unsigned)heap_caps_get_largest_free_block(CONTROLLER_DMA_CAPS));
}

/* v237: re-takes the part of the released reserve the burst did not use, so
 * other allocations cannot scatter into it before the controller closes.
 * The reserve plus the controller's allocations stay within the boot
 * reserve footprint. */
static void dma_reserve_settle(const char *why)
{
    if (s_dma_burst_released == 0u) {
        return;
    }
    const size_t free_now = heap_caps_get_free_size(CONTROLLER_DMA_CAPS);
    const size_t used =
        s_dma_burst_free > free_now ? s_dma_burst_free - free_now : 0u;
    const size_t spare =
        s_dma_burst_released > used ? s_dma_burst_released - used : 0u;
    s_dma_burst_released = 0u;
    size_t want = spare / CONTROLLER_DMA_RESERVE_CHUNK_BYTES;
    for (size_t i = 0u; i < CONTROLLER_DMA_RESERVE_CHUNKS && want > 0u; ++i) {
        if (s_dma_reserve[i]) {
            continue;
        }
        s_dma_reserve[i] = heap_caps_malloc(
            CONTROLLER_DMA_RESERVE_CHUNK_BYTES, CONTROLLER_DMA_CAPS);
        if (!s_dma_reserve[i]) {
            break;
        }
        --want;
    }
    ESP_LOGW(TAG, "USB DMA reserve: %s took ~%u B, %u/%u chunks re-taken "
                  "(DMA free=%u largest=%u)",
             why, (unsigned)used, dma_reserve_count(),
             (unsigned)CONTROLLER_DMA_RESERVE_CHUNKS,
             (unsigned)heap_caps_get_free_size(CONTROLLER_DMA_CAPS),
             (unsigned)heap_caps_get_largest_free_block(CONTROLLER_DMA_CAPS));
}

static void dma_reserve_topup_if_due(void)
{
    if (!CONTROLLER_DMA_RESERVE_ENABLED ||
        dma_reserve_count() == CONTROLLER_DMA_RESERVE_CHUNKS ||
        ticks_to_ms(xTaskGetTickCount() - s_dma_reserve_last_fill) <
            CONTROLLER_DMA_RESERVE_TOPUP_MS) {
        return;
    }
    dma_reserve_fill(NULL);
}

static void record_probe_result(controller_usb_probe_stage_t stage,
                                esp_err_t result)
{
    __atomic_store_n(&s_last_probe_stage, (uint8_t)stage, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_probe_result, (int32_t)result,
                     __ATOMIC_RELEASE);
}

static void begin_controller_fault_recovery(controller_state_t *state,
                                            const char *operation,
                                            esp_err_t error)
{
    /* v217: an unplug during streaming first surfaces as isoc/MIDI transfer
     * errors. A gone device needs a plain close, not a power-cycle, and the
     * DEV_GONE flag must never be cleared by a late fault report. */
    if (!state || state->device_gone ||
        !controller_usb_recovery_gate_begin_fault(&state->recovery_gate)) {
        return;
    }
    ESP_LOGW(TAG, "%s fault (%s); closing controller (root %u)",
             operation ? operation : "controller USB", esp_err_to_name(error),
             (unsigned)state->recovery_root);
    __atomic_store_n(&s_accepting_out, false, __ATOMIC_RELEASE);
    state->closing = true;
    state->midi_flush_attempted = false;
    controller_usb_audio_stream_request_stop(false);
}

/* Controller task only, with the controller closed. */
static void root_cycle_arm(controller_state_t *state, const char *why,
                           uint32_t first_delay_ms)
{
#if !CONTROLLER_ROOT_CYCLE_ENABLED
    ESP_LOGW(TAG, "recovery: %s; root power-cycle disabled (v239), waiting "
                  "for NEW_DEV", why);
    (void)first_delay_ms;
    return;
#endif
    /* v217: upstream hard-coded USB1 (index 1). On jc1060 index 1 is the
     * storage root; the controller root is the one it was probed on. */
    if (state->recovery_root == CONTROLLER_ROOT_UNKNOWN) {
        ESP_LOGW(TAG, "recovery: %s, controller root unknown, power-cycle "
                      "skipped", why);
        return;
    }
    state->root_cycle_pending = true;
    state->root_cycle_root = state->recovery_root;
    state->root_cycle_count = 0u;
    state->host_restart_count = 0u;
    state->root_cycle_at =
        xTaskGetTickCount() + pdMS_TO_TICKS(first_delay_ms);
    ESP_LOGW(TAG, "recovery: %s; power-cycling root %u in %u ms, repeated "
                  "until a device probes on it",
             why, (unsigned)state->root_cycle_root, (unsigned)first_delay_ms);
}

static void root_cycle_stop(controller_state_t *state, const char *why)
{
    if (!state->root_cycle_pending) {
        return;
    }
    state->root_cycle_pending = false;
    ESP_LOGW(TAG, "recovery: root %u power-cycle loop stopped after %u "
                  "cycles (%s)",
             (unsigned)state->root_cycle_root,
             (unsigned)state->root_cycle_count, why);
}

/* Blocks the controller task for SETTLE_MS (plus the POWER_ON retry); it
 * only runs while the controller is closed, so no MIDI or UAC is served. */
static void root_cycle_if_due(controller_state_t *state)
{
    if (!state->root_cycle_pending ||
        (int32_t)(xTaskGetTickCount() - state->root_cycle_at) < 0) {
        return;
    }
    const uint8_t root = state->root_cycle_root;
#if CONTROLLER_HOST_RESTART_ENABLED
    if (state->root_cycle_count >= CONTROLLER_HOST_RESTART_AFTER_CYCLES &&
        state->host_restart_count < CONTROLLER_HOST_RESTART_MAX) {
        state->host_restart_count++;
        const esp_err_t restart_rc =
            usb_host_manager_request_host_restart("controller root dead");
        ESP_LOGW(TAG, "recovery: root %u: %u power-cycles without NEW_DEV; "
                      "full USB host restart %u/%u: %s",
                 (unsigned)root, (unsigned)state->root_cycle_count,
                 (unsigned)state->host_restart_count,
                 (unsigned)CONTROLLER_HOST_RESTART_MAX,
                 esp_err_to_name(restart_rc));
        if (restart_rc == ESP_OK) {
            /* host_restart_participate() runs on the next loop pass. */
            state->root_cycle_count = 0u;
            state->root_cycle_at = xTaskGetTickCount() +
                pdMS_TO_TICKS(CONTROLLER_HOST_RESTART_SETTLE_MS);
            return;
        }
    }
#endif
    state->root_cycle_count++;
    const esp_err_t off_rc =
        usb_host_manager_set_root_power_by_index(root, false);
    esp_err_t on_rc = ESP_ERR_INVALID_STATE;
    if (off_rc == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(CONTROLLER_ROOT_CYCLE_SETTLE_MS));
        const TickType_t deadline = xTaskGetTickCount() +
            pdMS_TO_TICKS(CONTROLLER_ROOT_POWER_ON_TIMEOUT_MS);
        for (;;) {
            on_rc = usb_host_manager_set_root_power_by_index(root, true);
            if (on_rc != ESP_ERR_INVALID_STATE ||
                (int32_t)(deadline - xTaskGetTickCount()) <= 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(CONTROLLER_ROOT_POWER_ON_RETRY_MS));
        }
    }
    count_inc(&s_recovery_requests);
    const uint32_t next_ms =
        state->root_cycle_count < CONTROLLER_ROOT_CYCLE_MAX_CYCLES
            ? CONTROLLER_ROOT_CYCLE_RETRY_MS
            : CONTROLLER_ROOT_CYCLE_SLOW_MS;
    state->root_cycle_at = xTaskGetTickCount() + pdMS_TO_TICKS(next_ms);
    /* A failed power-off/on is retried by the next cycle: power-off of an
     * unpowered root is a no-op success, so a root left off is re-powered. */
    ESP_LOGW(TAG, "recovery: root %u power-cycle #%u off=%s on=%s; next in "
                  "%u ms unless a device probes on it",
             (unsigned)root, (unsigned)state->root_cycle_count,
             esp_err_to_name(off_rc),
             off_rc == ESP_OK ? esp_err_to_name(on_rc) : "skipped",
             (unsigned)next_ms);
}

static uint32_t next_backoff_ms(uint32_t delay_ms);

/* v239: first retry delay after the n-th fault in a row (faults less than
 * FAULT_STREAK_RESET_MS apart): 250 ms, doubling, capped at 5 s. */
static uint32_t fault_streak_delay_ms(uint32_t *streak, TickType_t *last_at)
{
    const TickType_t now = xTaskGetTickCount();
    if (*streak == 0u ||
        ticks_to_ms(now - *last_at) >= CONTROLLER_FAULT_STREAK_RESET_MS) {
        *streak = 0u;
    }
    *last_at = now;
    uint32_t delay_ms = CONTROLLER_REPROBE_FIRST_MS;
    for (uint32_t i = 0u; i < *streak && delay_ms < CONTROLLER_REPROBE_MAX_MS;
         ++i) {
        delay_ms = next_backoff_ms(delay_ms);
    }
    (*streak)++;
    return delay_ms;
}

#if !CONTROLLER_ROOT_CYCLE_ENABLED
/* v239: the faulted device is still enumerated at the same address (no
 * DEV_GONE), so it is re-probed like a v234 probe failure. open fails once
 * it is gone, which ends the re-probe. */
static void reprobe_after_fault(controller_state_t *state)
{
    const uint8_t address = state->fault_address;
    state->fault_address = 0u;
    if (address == 0u) {
        ESP_LOGW(TAG, "recovery: controller fault, no address to re-probe; "
                      "waiting for NEW_DEV");
        return;
    }
    const uint32_t delay_ms =
        fault_streak_delay_ms(&state->fault_streak, &state->fault_last_at);
    state->reprobe_pending = true;
    state->reprobe_address = address;
    state->reprobe_count = 0u;
    state->reprobe_delay_ms = delay_ms;
    state->reprobe_at = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    ESP_LOGW(TAG, "recovery: controller fault #%u; re-probing addr=%u in %u "
                  "ms (device still enumerated)",
             (unsigned)state->fault_streak, address, (unsigned)delay_ms);
}
#endif

static void submit_deferred_controller_recovery(controller_state_t *state)
{
    if (!state || !controller_usb_recovery_gate_pending(
            &state->recovery_gate)) {
        return;
    }
    controller_usb_recovery_gate_complete(&state->recovery_gate);
#if CONTROLLER_ROOT_CYCLE_ENABLED
    root_cycle_arm(state, "controller fault", 0u);
#else
    reprobe_after_fault(state);
#endif
}

static void usb_string_to_ascii(const usb_str_desc_t *desc, char *out,
                                size_t out_size)
{
    if (!out || out_size == 0u) {
        return;
    }
    out[0] = '\0';
    if (!desc || desc->bLength < 2u) {
        return;
    }
    size_t chars = (size_t)(desc->bLength - 2u) / 2u;
    if (chars >= out_size) {
        chars = out_size - 1u;
    }
    for (size_t i = 0u; i < chars; ++i) {
        const uint16_t code_unit = desc->wData[i];
        out[i] = code_unit >= 0x20u && code_unit <= 0x7Eu
                     ? (char)code_unit
                     : '?';
    }
    out[chars] = '\0';
}

static void publish_connection(bool connected)
{
    __atomic_store_n(&s_connected, connected, __ATOMIC_RELEASE);
    if (s_state.config.connection_cb) {
        s_state.config.connection_cb(connected,
                                     connected ? &s_state.identity : NULL,
                                     s_state.config.callback_ctx);
    }
}

static esp_err_t submit_in_if_idle(controller_state_t *state)
{
    if (!state->opened || !state->claimed || state->closing ||
        !state->in_transfer || state->in_active) {
        return ESP_OK;
    }
    state->in_transfer->device_handle = state->device;
    state->in_transfer->bEndpointAddress = state->identity.midi.in_ep_addr;
    state->in_transfer->num_bytes = usb_round_up_to_mps(
        DEFAULT_TRANSFER_BYTES, state->identity.midi.in_ep_mps);
    const esp_err_t rc = usb_host_transfer_submit(state->in_transfer);
    if (rc == ESP_OK) {
        state->in_active = true;
    } else {
        count_inc(&s_midi_in_submit_failures);
        begin_controller_fault_recovery(state, "MIDI IN submit", rc);
    }
    return rc;
}

static esp_err_t submit_out_if_idle(controller_state_t *state)
{
    if (!state->opened || !state->claimed || state->closing ||
        !state->out_transfer || state->out_active || !state->out_queue) {
        return ESP_OK;
    }

    const size_t capacity = state->out_transfer->data_buffer_size / 4u;
    size_t packets = 0u;
    controller_midi_out_item_t item;
    while (packets < capacity &&
           xQueueReceive(state->out_queue, &item, 0) == pdTRUE) {
        const uint32_t current_generation =
            __atomic_load_n(&s_out_generation, __ATOMIC_ACQUIRE);
        if (item.generation != current_generation ||
            !__atomic_load_n(&s_accepting_out, __ATOMIC_ACQUIRE)) {
            continue;
        }
        memcpy(&state->out_transfer->data_buffer[packets * 4u],
               item.packet, sizeof(item.packet));
        packets++;
    }
    if (packets == 0u) {
        return ESP_OK;
    }

    state->out_transfer->device_handle = state->device;
    state->out_transfer->bEndpointAddress = state->identity.midi.out_ep_addr;
    state->out_transfer->num_bytes = (int)(packets * 4u);
    const esp_err_t rc = usb_host_transfer_submit(state->out_transfer);
    if (s_midi_out_submit_logged < MIDI_OUT_LOG_LIMIT) {
        s_midi_out_submit_logged++;
        const uint8_t *b = state->out_transfer->data_buffer;
        ESP_LOGW(TAG, "MIDI OUT ep 0x%02x submit %u pkt rc=%s drops=%u",
                 state->identity.midi.out_ep_addr, (unsigned)packets,
                 esp_err_to_name(rc),
                 (unsigned)__atomic_load_n(&s_midi_out_queue_drops,
                                           __ATOMIC_RELAXED));
        /* v222: every packet of the logged transfers, to see deck 1 (0x90,
         * 0x97, 0xB0) messages actually leave. */
        for (size_t i = 0u; i < packets; ++i) {
            ESP_LOGW(TAG, "  MIDI OUT [%u] %02X %02X %02X %02X",
                     (unsigned)i, b[i * 4u], b[i * 4u + 1u], b[i * 4u + 2u],
                     b[i * 4u + 3u]);
        }
    }
    if (rc == ESP_OK) {
        state->out_active = true;
    } else {
        count_inc(&s_midi_out_submit_failures);
        begin_controller_fault_recovery(state, "MIDI OUT submit", rc);
    }
    return rc;
}

static void midi_in_callback(usb_transfer_t *transfer)
{
    controller_state_t *state = (controller_state_t *)transfer->context;
    state->in_active = false;

    const bool terminal =
        transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        transfer->status == USB_TRANSFER_STATUS_CANCELED;
    if (terminal) {
        vTaskPrioritySet(NULL, state->config.task_priority);
        state->closing = true;
        state->device_gone =
            state->device_gone ||
            transfer->status == USB_TRANSFER_STATUS_NO_DEVICE;
        if (state->device_gone) {
            controller_usb_recovery_gate_cancel(&state->recovery_gate);
        }
        __atomic_store_n(&s_accepting_out, false, __ATOMIC_RELEASE);
    }

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        (void)__atomic_add_fetch(&s_midi_bytes,
                                 (uint32_t)transfer->actual_num_bytes,
                                 __ATOMIC_RELAXED);
        for (int offset = 0; offset + 3 < transfer->actual_num_bytes;
             offset += 4) {
            usb_midi_message_t message;
            if (!usb_midi_parse_event_packet(&transfer->data_buffer[offset],
                                             &message)) {
                count_inc(&s_midi_parse_rejects);
                continue;
            }
            count_inc(&s_midi_packets);
            if (state->config.midi_cb) {
                state->config.midi_cb(&message,
                                      state->config.callback_ctx);
            }
        }
    } else if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
               transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "MIDI IN transfer status=%d", (int)transfer->status);
        begin_controller_fault_recovery(state, "MIDI IN transfer",
                                         ESP_FAIL);
    }

    if (!state->closing && !terminal) {
        const esp_err_t rc = submit_in_if_idle(state);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "MIDI IN resubmit: %s", esp_err_to_name(rc));
        }
    }
}

static void midi_out_callback(usb_transfer_t *transfer)
{
    controller_state_t *state = (controller_state_t *)transfer->context;
    state->out_active = false;
    if (s_midi_out_done_logged < MIDI_OUT_LOG_LIMIT) {
        s_midi_out_done_logged++;
        ESP_LOGW(TAG, "MIDI OUT done status=%d actual=%d/%d",
                 (int)transfer->status, transfer->actual_num_bytes,
                 transfer->num_bytes);
    }

    const bool terminal =
        transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        transfer->status == USB_TRANSFER_STATUS_CANCELED;
    if (terminal) {
        vTaskPrioritySet(NULL, state->config.task_priority);
        state->closing = true;
        state->device_gone =
            state->device_gone ||
            transfer->status == USB_TRANSFER_STATUS_NO_DEVICE;
        if (state->device_gone) {
            controller_usb_recovery_gate_cancel(&state->recovery_gate);
        }
        __atomic_store_n(&s_accepting_out, false, __ATOMIC_RELEASE);
    }

    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED &&
        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "MIDI OUT transfer status=%d", (int)transfer->status);
        begin_controller_fault_recovery(state, "MIDI OUT transfer",
                                         ESP_FAIL);
    }
    if (!state->closing && !terminal) {
        (void)submit_out_if_idle(state);
    }
}

static void close_wait_log(controller_state_t *state, const char *what)
{
    const TickType_t now = xTaskGetTickCount();
    if (ticks_to_ms(now - state->close_last_log) < CONTROLLER_CLOSE_LOG_MS) {
        return;
    }
    state->close_last_log = now;
    ESP_LOGW(TAG, "recovery: close waiting on %s for %u ms "
                  "(gone=%u uac_blockers=0x%02x in=%u out=%u claimed=%u "
                  "opened=%u)",
             what, (unsigned)ticks_to_ms(now - state->close_started),
             state->device_gone ? 1u : 0u,
             (unsigned)controller_usb_audio_stream_cleanup_blockers(),
             state->in_active ? 1u : 0u, state->out_active ? 1u : 0u,
             state->claimed ? 1u : 0u, state->opened ? 1u : 0u);
}

static void close_step(controller_state_t *state)
{
    const TickType_t now = xTaskGetTickCount();
    if (!state->close_timing) {
        state->close_timing = true;
        state->close_forced = false;
        state->close_started = now;
        state->close_last_log = now;
        state->close_uac_done = false;
        state->close_dma_free = heap_caps_get_free_size(CONTROLLER_DMA_CAPS);
        ESP_LOGW(TAG, "recovery: closing controller (gone=%u uac=%u "
                      "uac_blockers=0x%02x)",
                 state->device_gone ? 1u : 0u,
                 state->identity.usb_audio_active ? 1u : 0u,
                 (unsigned)controller_usb_audio_stream_cleanup_blockers());
    }
    if (!state->close_forced &&
        ticks_to_ms(now - state->close_started) >= CONTROLLER_CLOSE_FORCE_MS) {
        state->close_forced = true;
        state->midi_flush_attempted = false;
        ESP_LOGW(TAG, "recovery: close stalled %u ms, forcing endpoint "
                      "halt/flush (gone=%u)",
                 (unsigned)ticks_to_ms(now - state->close_started),
                 state->device_gone ? 1u : 0u);
        controller_usb_audio_stream_force_flush();
    }
    state->closing = true;
    __atomic_store_n(&s_accepting_out, false, __ATOMIC_RELEASE);
    if (!state->out_generation_closed) {
        (void)__atomic_add_fetch(&s_out_generation, 1u, __ATOMIC_ACQ_REL);
        state->out_generation_closed = true;
    }
    controller_usb_audio_stream_request_stop(state->device_gone);
    if (!controller_usb_audio_stream_poll_cleanup()) {
        close_wait_log(state, "UAC cleanup");
        return;
    }
    if (!state->close_uac_done) {
        state->close_uac_done = true;
        state->close_uac_done_at = now;
    }
    if (state->out_queue) {
        (void)xQueueReset(state->out_queue);
    }
    if ((!state->device_gone || state->close_forced) && state->claimed &&
        state->device && (state->in_active || state->out_active) &&
        !state->midi_flush_attempted) {
        state->midi_flush_attempted = true;
        const uint8_t endpoints[] = {
            state->identity.midi.in_ep_addr,
            state->identity.midi.out_ep_addr,
        };
        const bool active[] = { state->in_active, state->out_active };
        for (size_t i = 0u; i < 2u; ++i) {
            if (!active[i]) {
                continue;
            }
            const esp_err_t halt_rc =
                usb_host_endpoint_halt(state->device, endpoints[i]);
            if (halt_rc == ESP_OK || halt_rc == ESP_ERR_INVALID_STATE) {
                const esp_err_t flush_rc =
                    usb_host_endpoint_flush(state->device, endpoints[i]);
                if (flush_rc != ESP_OK && flush_rc != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "flush MIDI endpoint 0x%02X: %s",
                             endpoints[i], esp_err_to_name(flush_rc));
                }
            } else {
                ESP_LOGW(TAG, "halt MIDI endpoint 0x%02X: %s",
                         endpoints[i], esp_err_to_name(halt_rc));
            }
        }
    }
    if (state->in_active || state->out_active) {
        close_wait_log(state, "MIDI transfers");
        return;
    }
    if (state->in_transfer) {
        if (usb_host_transfer_free(state->in_transfer) != ESP_OK) {
            close_wait_log(state, "MIDI IN free");
            return;
        }
        state->in_transfer = NULL;
    }
    if (state->out_transfer) {
        if (usb_host_transfer_free(state->out_transfer) != ESP_OK) {
            close_wait_log(state, "MIDI OUT free");
            return;
        }
        state->out_transfer = NULL;
    }
    if (state->claimed) {
        const esp_err_t rc = usb_host_interface_release(
            state->client, state->device,
            state->identity.midi.interface_num);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "release MIDI interface: %s", esp_err_to_name(rc));
            close_wait_log(state, "MIDI release");
            return;
        }
        state->claimed = false;
    }
    if (state->opened) {
        if (ticks_to_ms(now - state->close_uac_done_at) <
            CONTROLLER_CLOSE_EP0_SETTLE_MS) {
            /* No log: normal, one or two loop passes. */
            return;
        }
        const esp_err_t rc =
            usb_host_device_close(state->client, state->device);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "close controller device: %s", esp_err_to_name(rc));
            close_wait_log(state, "device close");
            return;
        }
        state->opened = false;
        state->device = NULL;
    }

    const bool was_connected =
        __atomic_exchange_n(&s_connected, false, __ATOMIC_ACQ_REL);
    const bool was_gone = state->device_gone;
    ESP_LOGW(TAG, "recovery: controller closed in %u ms (gone=%u forced=%u); "
                  "root %u free for re-enumeration; DMA freed ~%d B",
             (unsigned)ticks_to_ms(now - state->close_started),
             was_gone ? 1u : 0u, state->close_forced ? 1u : 0u,
             (unsigned)state->recovery_root,
             (int)heap_caps_get_free_size(CONTROLLER_DMA_CAPS) -
                 (int)state->close_dma_free);
    dma_reserve_fill("controller closed");
    state->close_timing = false;
    state->close_forced = false;
    state->close_uac_done = false;
    state->uac_retry_pending = false;
    const bool faulted =
        controller_usb_recovery_gate_pending(&state->recovery_gate);
    if (was_gone || faulted) {
        state->awaiting_reattach = true;
        state->detached_at = now;
    }
    if (was_gone && !faulted) {
        /* v237: an unplugged controller normally comes back with its own
         * NEW_DEV; the loop only covers a root that stays dead. */
        root_cycle_arm(state, "controller unplugged",
                       CONTROLLER_ROOT_CYCLE_RETRY_MS);
        state->fault_streak = 0u;
        state->uac_fault_streak = 0u;
    }
    state->fault_address = faulted && !was_gone ? state->identity.address : 0u;
    memset(&state->identity, 0, sizeof(state->identity));
    state->closing = false;
    state->device_gone = false;
    state->out_generation_closed = false;
    state->midi_flush_attempted = false;
    if (was_connected) {
        count_inc(&s_midi_disconnects);
        if (state->config.connection_cb) {
            state->config.connection_cb(false, NULL,
                                        state->config.callback_ctx);
        }
        ESP_LOGI(TAG, "USB-MIDI controller disconnected");
    }
    submit_deferred_controller_recovery(state);
}

/* jc1060 diagnostic, v214: logged on every UAC start, not only on failure,
 * so the raw interface/endpoint/class-specific bytes behind the selected
 * format are always in the HIL log (DDJ-400 16 vs 24-bit question). Each
 * descriptor is printed whole, up to 16 bytes. WARN level:
 * CONFIG_LOG_DEFAULT_LEVEL=2 compiles out INFO. */
static void dump_uac_descriptors(const usb_config_desc_t *config_desc)
{
    const uint8_t *raw = (const uint8_t *)config_desc;
    const size_t total = config_desc->wTotalLength;
    ESP_LOGW(TAG, "UAC desc dump total=%u:", (unsigned)total);
    for (size_t off = 0u; off + 2u <= total;) {
        const uint8_t dlen = raw[off];
        if (dlen < 2u || off + dlen > total) {
            break;
        }
        const uint8_t dtype = raw[off + 1u];
        if (dtype == 0x04u || dtype == 0x05u || dtype == 0x24u ||
            dtype == 0x25u) {
            char hex[16u * 3u + 1u];
            size_t pos = 0u;
            for (size_t k = 0u; k < dlen && k < 16u; ++k) {
                static const char digits[] = "0123456789abcdef";
                hex[pos++] = digits[raw[off + k] >> 4];
                hex[pos++] = digits[raw[off + k] & 0x0fu];
                hex[pos++] = ' ';
            }
            hex[pos > 0u ? pos - 1u : 0u] = '\0';
            ESP_LOGW(TAG, "  @%u len %u: %s", (unsigned)off, dlen, hex);
        }
        off += dlen;
    }
}

static uint32_t next_backoff_ms(uint32_t delay_ms)
{
    if (delay_ms >= CONTROLLER_REPROBE_MAX_MS / 2u) {
        return CONTROLLER_REPROBE_MAX_MS;
    }
    return delay_ms * 2u;
}

static bool retry_log_due(uint32_t count)
{
    return count <= 3u || count % CONTROLLER_REPROBE_LOG_EVERY == 0u;
}

/* The replug NO_MEM comes from the internal DMA heap (HCD qTD lists, URB
 * buffers); logged with each retry so a HIL run shows how short it was. */
static void log_retry(const char *what, uint8_t address, esp_err_t rc,
                      unsigned stage, uint32_t count, uint32_t delay_ms)
{
    const uint32_t dma_caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    ESP_LOGW(TAG, "%s addr=%u: %s at stage %u; retry #%u in %u ms "
                  "(internal free=%u largest=%u, DMA free=%u largest=%u)",
             what, address, esp_err_to_name(rc), stage, (unsigned)count,
             (unsigned)delay_ms,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(dma_caps),
             (unsigned)heap_caps_get_largest_free_block(dma_caps));
}

static esp_err_t start_uac(controller_state_t *state,
                           const usb_config_desc_t *config_desc)
{
    const esp_err_t rc = controller_usb_audio_stream_start(
        state->client, state->device, (const uint8_t *)config_desc,
        config_desc->wTotalLength, xTaskGetCurrentTaskHandle(),
        CONTROLLER_USB_ACTIVE_PRIORITY, state->config.task_priority);
    state->identity.usb_audio_active = rc == ESP_OK;
    return rc;
}

static void schedule_uac_retry(controller_state_t *state, esp_err_t rc)
{
    if (rc == ESP_ERR_NOT_SUPPORTED || rc == ESP_ERR_INVALID_ARG) {
        /* No usable format: retrying cannot help. */
        state->uac_retry_pending = false;
        return;
    }
    if (!state->uac_retry_pending) {
        state->uac_retry_pending = true;
        state->uac_retry_count = 0u;
        state->uac_retry_delay_ms = CONTROLLER_REPROBE_FIRST_MS;
    } else {
        state->uac_retry_delay_ms =
            next_backoff_ms(state->uac_retry_delay_ms);
    }
    state->uac_retry_count++;
    state->uac_retry_at =
        xTaskGetTickCount() + pdMS_TO_TICKS(state->uac_retry_delay_ms);
    if (retry_log_due(state->uac_retry_count)) {
        /* Stage: 0 = claim/config start refused synchronously. Control
         * step failures (SET_INTERFACE, rate) log as "control step N". */
        log_retry("UAC start", state->identity.address, rc, 0u,
                  state->uac_retry_count, state->uac_retry_delay_ms);
    }
}

/* v239: HIL v236 closed the whole controller on a UAC fault, then the
 * recovery lost the root. Now only the stream stops: poll_cleanup() frees
 * its URBs and releases the interface, and the v235 retry starts it again
 * (SET_INTERFACE alt 0 first). MIDI and the device handle stay up. */
static void restart_uac_in_place(controller_state_t *state, esp_err_t fault_rc,
                                 uint32_t start_seq)
{
    if (state->device_gone || state->closing) {
        return;
    }
    state->identity.usb_audio_active = false;
    controller_usb_audio_stream_request_stop(false);
    const uint32_t delay_ms = fault_streak_delay_ms(
        &state->uac_fault_streak, &state->uac_fault_last_at);
    state->uac_retry_pending = true;
    state->uac_retry_count = 0u;
    state->uac_retry_delay_ms = delay_ms;
    state->uac_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    ESP_LOGW(TAG, "UAC stream fault (%s, seq %u) addr=%u, fault #%u; "
                  "restarting the stream in place in %u ms, MIDI stays up",
             esp_err_to_name(fault_rc), (unsigned)start_seq,
             state->identity.address, (unsigned)state->uac_fault_streak,
             (unsigned)delay_ms);
}

static esp_err_t probe_device(controller_state_t *state, uint8_t address)
{
    count_inc(&s_devices_probed);
    __atomic_store_n(&s_last_probe_address, address, __ATOMIC_RELEASE);
    record_probe_result(CONTROLLER_USB_PROBE_OPEN, ESP_ERR_INVALID_STATE);
    state->probe_root = CONTROLLER_ROOT_UNKNOWN;
    usb_device_handle_t device = NULL;
    esp_err_t rc = usb_host_device_open(state->client, address, &device);
    if (rc != ESP_OK) {
        record_probe_result(CONTROLLER_USB_PROBE_OPEN, rc);
        return rc;
    }

    usb_device_info_t info = {0};
    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;
    rc = usb_host_device_info(device, &info);
    if (rc != ESP_OK) {
        record_probe_result(CONTROLLER_USB_PROBE_DEVICE_INFO, rc);
    } else {
        __atomic_store_n(&s_last_parent_port, info.parent.port_num,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_last_direct_root, info.parent.dev_hdl == NULL,
                         __ATOMIC_RELEASE);
        if (info.parent.dev_hdl == NULL) {
            state->probe_root = info.parent.port_num;
        }
        rc = usb_host_get_device_descriptor(device, &device_desc);
        if (rc != ESP_OK) {
            record_probe_result(CONTROLLER_USB_PROBE_DEVICE_DESCRIPTOR, rc);
        }
    }
    if (rc == ESP_OK) {
        __atomic_store_n(&s_last_seen_vid, device_desc->idVendor,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_last_seen_pid, device_desc->idProduct,
                         __ATOMIC_RELEASE);
        rc = usb_host_get_active_config_descriptor(device, &config_desc);
        if (rc != ESP_OK) {
            record_probe_result(CONTROLLER_USB_PROBE_CONFIG_DESCRIPTOR, rc);
        }
    }
    if (rc != ESP_OK || !device_desc || !config_desc) {
        count_inc(&s_descriptor_rejects);
        if (rc == ESP_OK) {
            record_probe_result(CONTROLLER_USB_PROBE_CONFIG_DESCRIPTOR,
                                ESP_FAIL);
        }
        (void)usb_host_device_close(state->client, device);
        return rc == ESP_OK ? ESP_FAIL : rc;
    }
    __atomic_store_n(&s_last_config_total_length, config_desc->wTotalLength,
                     __ATOMIC_RELEASE);

    usb_midi_endpoints_t endpoints;
    if (!usb_midi_find_streaming_endpoints((const uint8_t *)config_desc,
                                           config_desc->wTotalLength,
                                           &endpoints)) {
        count_inc(&s_midi_descriptor_rejects);
        record_probe_result(CONTROLLER_USB_PROBE_MIDI_DESCRIPTOR,
                            ESP_ERR_NOT_FOUND);
        (void)usb_host_device_close(state->client, device);
        return ESP_ERR_NOT_FOUND;
    }
    if (state->opened || state->claimed) {
        record_probe_result(CONTROLLER_USB_PROBE_ALREADY_OWNED,
                            ESP_ERR_INVALID_STATE);
        (void)usb_host_device_close(state->client, device);
        return ESP_ERR_INVALID_STATE;
    }

    state->device = device;
    state->opened = true;
    state->closing = false;
    s_midi_out_submit_logged = 0u;
    s_midi_out_done_logged = 0u;
    state->device_gone = false;
    state->midi_flush_attempted = false;
    state->uac_retry_pending = false;
    controller_usb_recovery_gate_cancel(&state->recovery_gate);
    state->recovery_root =
        info.parent.dev_hdl == NULL &&
                info.parent.port_num < USB_HOST_RECOVERY_PORT_COUNT
            ? info.parent.port_num
            : CONTROLLER_ROOT_UNKNOWN;
    state->identity = (controller_usb_identity_t) {
        .vid = device_desc->idVendor,
        .pid = device_desc->idProduct,
        .address = address,
        .speed = (uint8_t)info.speed,
        .parent_port = info.parent.port_num,
        .direct_root_child = info.parent.dev_hdl == NULL,
        .midi = endpoints,
    };
    usb_string_to_ascii(info.str_desc_product, state->identity.product,
                        sizeof(state->identity.product));

    /* v236: first DMA allocation of this probe; every failure from here on
     * goes through close_step(), which re-takes the reserve. v237: the burst
     * runs to the end of the UAC start (isoc URBs included). */
    dma_reserve_release("probe");
    rc = usb_host_interface_claim(state->client, state->device,
                                  endpoints.interface_num,
                                  endpoints.alternate_setting);
    if (rc != ESP_OK) {
        count_inc(&s_interface_claim_failures);
        record_probe_result(CONTROLLER_USB_PROBE_INTERFACE_CLAIM, rc);
        state->closing = true;
        close_step(state);
        return rc;
    }
    state->claimed = true;

    const int in_bytes = usb_round_up_to_mps(DEFAULT_TRANSFER_BYTES,
                                             endpoints.in_ep_mps);
    const int out_bytes = usb_round_up_to_mps(DEFAULT_TRANSFER_BYTES,
                                              endpoints.out_ep_mps);
    rc = usb_host_transfer_alloc(in_bytes, 0, &state->in_transfer);
    if (rc == ESP_OK) {
        rc = usb_host_transfer_alloc(out_bytes, 0, &state->out_transfer);
    }
    if (rc != ESP_OK) {
        count_inc(&s_transfer_alloc_failures);
        record_probe_result(CONTROLLER_USB_PROBE_TRANSFER_ALLOC, rc);
        state->closing = true;
        close_step(state);
        return rc;
    }

    state->in_transfer->device_handle = state->device;
    state->in_transfer->bEndpointAddress = endpoints.in_ep_addr;
    state->in_transfer->callback = midi_in_callback;
    state->in_transfer->context = state;
    state->out_transfer->device_handle = state->device;
    state->out_transfer->bEndpointAddress = endpoints.out_ep_addr;
    state->out_transfer->callback = midi_out_callback;
    state->out_transfer->context = state;

    rc = submit_in_if_idle(state);
    if (rc != ESP_OK) {
        record_probe_result(CONTROLLER_USB_PROBE_IN_SUBMIT, rc);
        state->closing = true;
        close_step(state);
        return rc;
    }

    const bool uac_device = state->identity.vid == FLX4_USB_VID &&
        (state->identity.pid == FLX4_USB_PID ||
         state->identity.pid == DDJ400_USB_PID);
    const bool uac_port = state->identity.direct_root_child &&
        (state->identity.parent_port == CONTROLLER_UAC_ROOT_PORT ||
         state->identity.parent_port == CONTROLLER_UAC_LEGACY_PORT);
    if (uac_device) {
        /* WARN: CONFIG_LOG_DEFAULT_LEVEL=2 compiles out the INFO ready line,
         * and a skipped UAC start used to leave no trace at all. */
        ESP_LOGW(TAG, "UAC gate PID=0x%04X parent_port=%u direct_root=%u -> %s",
                 state->identity.pid, state->identity.parent_port,
                 state->identity.direct_root_child ? 1u : 0u,
                 uac_port ? "start" : "skip");
    }
    if (uac_device && uac_port) {
        const bool is_ddj400 = state->identity.pid == DDJ400_USB_PID;
        dump_uac_descriptors(config_desc);
        const esp_err_t audio_rc = start_uac(state, config_desc);
        if (audio_rc != ESP_OK) {
            ESP_LOGW(TAG, "%s UAC unavailable; MIDI remains active: %s",
                     is_ddj400 ? "DDJ-400" : "FLX4",
                     esp_err_to_name(audio_rc));
            schedule_uac_retry(state, audio_rc);
        }
    }
    dma_reserve_settle("probe");

    if (state->awaiting_reattach) {
        state->awaiting_reattach = false;
        ESP_LOGW(TAG, "recovery: controller re-attached after %u ms "
                      "(addr=%u root=%u power-cycles=%u uac=%u)",
                 (unsigned)ticks_to_ms(xTaskGetTickCount() -
                                       state->detached_at),
                 address, (unsigned)state->recovery_root,
                 (unsigned)state->root_cycle_count,
                 state->identity.usb_audio_active ? 1u : 0u);
    }
    count_inc(&s_midi_connects);
    (void)__atomic_add_fetch(&s_out_generation, 1u, __ATOMIC_ACQ_REL);
    state->out_generation_closed = false;
    __atomic_store_n(&s_accepting_out, true, __ATOMIC_RELEASE);
    publish_connection(true);
    record_probe_result(CONTROLLER_USB_PROBE_READY, ESP_OK);
    ESP_LOGI(TAG,
             "USB-MIDI ready addr=%u VID=0x%04X PID=0x%04X intf=%u "
             "alt=%u IN=0x%02X/%u OUT=0x%02X/%u parent_port=%u direct_root=%u",
             address, state->identity.vid, state->identity.pid,
             endpoints.interface_num, endpoints.alternate_setting,
             endpoints.in_ep_addr, endpoints.in_ep_mps,
             endpoints.out_ep_addr, endpoints.out_ep_mps,
             state->identity.parent_port,
             state->identity.direct_root_child ? 1u : 0u);
    return ESP_OK;
}

static void client_event_callback(const usb_host_client_event_msg_t *event_msg,
                                  void *arg)
{
    controller_state_t *state = (controller_state_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGW(TAG, "recovery: NEW_DEV addr=%u (opened=%u closing=%u)",
                 (unsigned)event_msg->new_dev.address,
                 state->opened ? 1u : 0u, state->closing ? 1u : 0u);
        if (!state->probe_queue ||
            xQueueSend(state->probe_queue, &event_msg->new_dev.address, 0) !=
                pdTRUE) {
            count_inc(&s_probe_event_drops);
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (state->opened && event_msg->dev_gone.dev_hdl == state->device) {
            ESP_LOGW(TAG, "recovery: DEV_GONE controller (uac_blockers=0x%02x)",
                     (unsigned)controller_usb_audio_stream_cleanup_blockers());
            state->closing = true;
            state->device_gone = true;
            controller_usb_recovery_gate_cancel(&state->recovery_gate);
            __atomic_store_n(&s_accepting_out, false, __ATOMIC_RELEASE);
            controller_usb_audio_stream_request_stop(true);
        }
        break;
    default:
        break;
    }
}

static bool probe_failure_is_retryable(esp_err_t rc)
{
    /* NOT_FOUND: not a MIDI device, or gone (open failed).
     * INVALID_STATE: already owned, or not configured any more. */
    return rc != ESP_OK && rc != ESP_ERR_NOT_FOUND &&
           rc != ESP_ERR_INVALID_STATE;
}

static void run_probe(controller_state_t *state, uint8_t address)
{
    const bool retry =
        state->reprobe_pending && state->reprobe_address == address;
    const esp_err_t rc = probe_device(state, address);
    if (state->root_cycle_pending &&
        (rc == ESP_OK || state->probe_root == state->root_cycle_root)) {
        /* v237: something enumerated on the recovered root (NEW_DEV). A
         * failed MIDI probe there is the re-probe's job, not a power-cycle. */
        root_cycle_stop(state, "NEW_DEV probed on it");
    }
    if (rc == ESP_OK) {
        if (retry) {
            ESP_LOGW(TAG, "recovery: probe addr=%u succeeded on retry %u",
                     address, (unsigned)state->reprobe_count);
        }
        state->reprobe_pending = false;
        return;
    }
    if (!probe_failure_is_retryable(rc)) {
        if (retry) {
            ESP_LOGW(TAG, "recovery: re-probe addr=%u stopped after %u "
                          "retries: %s",
                     address, (unsigned)state->reprobe_count,
                     esp_err_to_name(rc));
            state->reprobe_pending = false;
        }
        return;
    }
    if (!retry) {
        state->reprobe_pending = true;
        state->reprobe_address = address;
        state->reprobe_count = 0u;
        state->reprobe_delay_ms = CONTROLLER_REPROBE_FIRST_MS;
    } else {
        state->reprobe_delay_ms = next_backoff_ms(state->reprobe_delay_ms);
    }
    state->reprobe_count++;
    state->reprobe_at =
        xTaskGetTickCount() + pdMS_TO_TICKS(state->reprobe_delay_ms);
    if (retry_log_due(state->reprobe_count)) {
        log_retry("probe", address, rc,
                  __atomic_load_n(&s_last_probe_stage, __ATOMIC_ACQUIRE),
                  state->reprobe_count, state->reprobe_delay_ms);
    }
}

static void reprobe_if_due(controller_state_t *state)
{
    if (!state->reprobe_pending ||
        (int32_t)(xTaskGetTickCount() - state->reprobe_at) < 0) {
        return;
    }
    run_probe(state, state->reprobe_address);
}

static void retry_uac_if_due(controller_state_t *state)
{
    if (!state->uac_retry_pending ||
        (int32_t)(xTaskGetTickCount() - state->uac_retry_at) < 0) {
        return;
    }
    if (!controller_usb_audio_stream_is_quiesced()) {
        /* A start that failed after its claim is still being released by
         * poll_cleanup(); check again shortly without counting a retry. */
        state->uac_retry_at =
            xTaskGetTickCount() + pdMS_TO_TICKS(CONTROLLER_REPROBE_FIRST_MS);
        return;
    }
    const usb_config_desc_t *config_desc = NULL;
    esp_err_t rc =
        usb_host_get_active_config_descriptor(state->device, &config_desc);
    if (rc == ESP_OK) {
        dma_reserve_release("UAC retry");
        rc = start_uac(state, config_desc);
        dma_reserve_settle("UAC retry");
    }
    if (rc == ESP_OK) {
        state->uac_retry_pending = false;
        ESP_LOGW(TAG, "recovery: UAC start addr=%u succeeded on retry %u",
                 state->identity.address, (unsigned)state->uac_retry_count);
        return;
    }
    schedule_uac_retry(state, rc);
}

#if CONTROLLER_HOST_RESTART_ENABLED
/* v238: controller side of usb_host_manager_request_host_restart(). Closes
 * the controller if it is open, deregisters the client, waits for the
 * reinstalled stack, registers a new client and re-powers its root. */
static void host_restart_participate(
    controller_state_t *state,
    const usb_host_client_config_t *client_config)
{
    const uint32_t generation = usb_host_manager_host_generation();
    if (state->opened && !state->closing) {
        ESP_LOGW(TAG, "host restart: closing controller addr=%u",
                 state->identity.address);
        __atomic_store_n(&s_accepting_out, false, __ATOMIC_RELEASE);
        state->closing = true;
        state->midi_flush_attempted = false;
        controller_usb_audio_stream_request_stop(false);
    }
    while (state->closing && usb_host_manager_host_restart_pending()) {
        (void)usb_host_client_handle_events(state->client, pdMS_TO_TICKS(10));
        close_step(state);
    }
    if (state->opened) {
        /* The manager gave up waiting and kept the old stack. */
        ESP_LOGW(TAG, "host restart: controller still open, not released");
        return;
    }
    state->reprobe_pending = false;
    state->uac_retry_pending = false;
    const esp_err_t dereg_rc = usb_host_client_deregister(state->client);
    if (dereg_rc != ESP_OK) {
        ESP_LOGW(TAG, "host restart: client deregister: %s",
                 esp_err_to_name(dereg_rc));
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    state->client = NULL;
    __atomic_store_n(&s_registered, false, __ATOMIC_RELEASE);
    usb_host_manager_host_restart_release(s_restart_participant, generation);

    esp_err_t rc;
    while ((rc = usb_host_manager_wait_host_restart(
                generation, pdMS_TO_TICKS(CONTROLLER_HOST_RESTART_WAIT_MS))) ==
           ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "host restart: still waiting for the USB host stack");
    }
    /* Addresses queued before the restart belong to the old stack. */
    (void)xQueueReset(state->probe_queue);
    bool logged = false;
    for (;;) {
        rc = usb_host_client_register(client_config, &state->client);
        if (rc == ESP_OK) {
            break;
        }
        state->client = NULL;
        if (!logged) {
            ESP_LOGE(TAG, "host restart: client register: %s",
                     esp_err_to_name(rc));
            logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(CONTROLLER_REGISTER_RETRY_MS));
    }
    __atomic_store_n(&s_registered, true, __ATOMIC_RELEASE);
    const esp_err_t power_rc = usb_host_manager_set_root_power_by_index(
        state->config.root_port_index, true);
    ESP_LOGW(TAG, "host restart: client re-registered, root %u power-on: %s",
             (unsigned)state->config.root_port_index,
             esp_err_to_name(power_rc));
    if (state->root_cycle_pending) {
        state->root_cycle_count = 0u;
        state->root_cycle_at = xTaskGetTickCount() +
            pdMS_TO_TICKS(CONTROLLER_HOST_RESTART_SETTLE_MS);
    }
}
#endif

static void controller_task(void *arg)
{
    TaskHandle_t starter = (TaskHandle_t)arg;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = s_state.config.max_event_messages,
        .flags = {
            .notify_dev_removed = 1u,
        },
        .async = {
            .client_event_callback = client_event_callback,
            .callback_arg = &s_state,
        },
    };

    s_register_result =
        usb_host_client_register(&client_config, &s_state.client);
    __atomic_store_n(&s_registered, s_register_result == ESP_OK,
                     __ATOMIC_RELEASE);
    xTaskNotifyGive(starter);
    if (s_register_result != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
#if CONTROLLER_HOST_RESTART_ENABLED
    const esp_err_t participant_rc =
        usb_host_manager_restart_participant_register("controller",
                                                      &s_restart_participant);
    if (participant_rc != ESP_OK) {
        /* The manager then does not wait for this client and refuses the
         * uninstall while it is registered: restarts keep the old stack. */
        ESP_LOGW(TAG, "host restart participant: %s",
                 esp_err_to_name(participant_rc));
    }
#endif

    for (;;) {
        const esp_err_t rc = usb_host_client_handle_events(
            s_state.client, pdMS_TO_TICKS(100));
        if (rc != ESP_OK && rc != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "client events: %s", esp_err_to_name(rc));
        }
#if CONTROLLER_HOST_RESTART_ENABLED
        if (s_restart_participant != 0u &&
            usb_host_manager_host_restart_pending()) {
            host_restart_participate(&s_state, &client_config);
            continue;
        }
#endif
        if (s_state.closing) {
            close_step(&s_state);
            continue;
        }
        if (!s_state.opened) {
            dma_reserve_topup_if_due();
        }
        controller_usb_audio_stream_stats_t audio_stats = {0};
        controller_usb_audio_stream_get_stats(&audio_stats);
        if (s_state.opened && s_state.identity.usb_audio_active &&
            audio_stats.faulted) {
            restart_uac_in_place(&s_state, (esp_err_t)audio_stats.fault_rc,
                                 audio_stats.start_seq);
        }
        controller_usb_audio_stream_log_trace();
        (void)controller_usb_audio_stream_poll_cleanup();
        if (s_state.opened) {
            retry_uac_if_due(&s_state);
        }
        uint8_t address = 0u;
        while (!s_state.closing && s_state.probe_queue &&
               xQueueReceive(s_state.probe_queue, &address, 0) == pdTRUE) {
            run_probe(&s_state, address);
        }
        if (!s_state.closing && !s_state.opened) {
            reprobe_if_due(&s_state);
            /* v237: after the queued probes, so a pending NEW_DEV on the
             * root stops the loop before another power-cycle. */
            if (!s_state.opened) {
                root_cycle_if_due(&s_state);
            }
        }
        (void)submit_in_if_idle(&s_state);
        (void)submit_out_if_idle(&s_state);
    }
}

esp_err_t controller_usb_host_init(const controller_usb_host_config_t *config)
{
    if (!config || config->task_stack_size < 4096u ||
        config->task_priority == 0u || config->midi_out_queue_depth == 0u ||
        config->max_event_messages < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_manager_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (__atomic_load_n(&s_registered, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }

    memset(&s_state, 0, sizeof(s_state));
    dma_reserve_fill("boot");
    s_state.recovery_root = CONTROLLER_ROOT_UNKNOWN;
    controller_usb_recovery_gate_init(&s_state.recovery_gate);
    s_state.config = *config;
    s_state.out_queue = xQueueCreate(config->midi_out_queue_depth,
                                     sizeof(controller_midi_out_item_t));
    s_state.probe_queue = xQueueCreate((UBaseType_t)config->max_event_messages,
                                       sizeof(uint8_t));
    if (!s_state.out_queue || !s_state.probe_queue) {
        if (s_state.out_queue) {
            vQueueDelete(s_state.out_queue);
            s_state.out_queue = NULL;
        }
        if (s_state.probe_queue) {
            vQueueDelete(s_state.probe_queue);
            s_state.probe_queue = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    const TaskHandle_t starter = xTaskGetCurrentTaskHandle();
    BaseType_t created;
    if (config->task_core_id == tskNO_AFFINITY) {
        created = xTaskCreate(controller_task, "controller_usb",
                              config->task_stack_size, (void *)starter,
                              config->task_priority, NULL);
    } else {
        created = xTaskCreatePinnedToCore(
            controller_task, "controller_usb", config->task_stack_size,
            (void *)starter, config->task_priority, NULL,
            config->task_core_id);
    }
    if (created != pdPASS) {
        vQueueDelete(s_state.out_queue);
        s_state.out_queue = NULL;
        vQueueDelete(s_state.probe_queue);
        s_state.probe_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0u) {
        return ESP_ERR_TIMEOUT;
    }
    return s_register_result;
}

esp_err_t controller_usb_host_send_packet(const uint8_t packet[4])
{
    if (!packet) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!__atomic_load_n(&s_accepting_out, __ATOMIC_ACQUIRE) ||
        !s_state.out_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    controller_midi_out_item_t item = {
        .generation = __atomic_load_n(&s_out_generation, __ATOMIC_ACQUIRE),
    };
    memcpy(item.packet, packet, sizeof(item.packet));
    if (xQueueSend(s_state.out_queue, &item, 0) != pdTRUE) {
        count_inc(&s_midi_out_queue_drops);
        return ESP_ERR_TIMEOUT;
    }
    if (s_state.client) {
        (void)usb_host_client_unblock(s_state.client);
    }
    return ESP_OK;
}

bool controller_usb_host_is_connected(void)
{
    return __atomic_load_n(&s_connected, __ATOMIC_ACQUIRE);
}

bool controller_usb_host_get_identity(controller_usb_identity_t *identity_out)
{
    if (!identity_out || !controller_usb_host_is_connected()) {
        return false;
    }
    *identity_out = s_state.identity;
    return true;
}

void controller_usb_host_get_diagnostics(
    controller_usb_host_diagnostics_t *diag_out)
{
    if (!diag_out) {
        return;
    }
    *diag_out = (controller_usb_host_diagnostics_t) {
        .devices_probed =
            __atomic_load_n(&s_devices_probed, __ATOMIC_ACQUIRE),
        .descriptor_rejects =
            __atomic_load_n(&s_descriptor_rejects, __ATOMIC_ACQUIRE),
        .midi_descriptor_rejects =
            __atomic_load_n(&s_midi_descriptor_rejects, __ATOMIC_ACQUIRE),
        .interface_claim_failures =
            __atomic_load_n(&s_interface_claim_failures, __ATOMIC_ACQUIRE),
        .transfer_alloc_failures =
            __atomic_load_n(&s_transfer_alloc_failures, __ATOMIC_ACQUIRE),
        .midi_connects =
            __atomic_load_n(&s_midi_connects, __ATOMIC_ACQUIRE),
        .midi_disconnects =
            __atomic_load_n(&s_midi_disconnects, __ATOMIC_ACQUIRE),
        .midi_packets =
            __atomic_load_n(&s_midi_packets, __ATOMIC_ACQUIRE),
        .midi_bytes =
            __atomic_load_n(&s_midi_bytes, __ATOMIC_ACQUIRE),
        .midi_parse_rejects =
            __atomic_load_n(&s_midi_parse_rejects, __ATOMIC_ACQUIRE),
        .midi_in_submit_failures =
            __atomic_load_n(&s_midi_in_submit_failures, __ATOMIC_ACQUIRE),
        .midi_out_submit_failures =
            __atomic_load_n(&s_midi_out_submit_failures, __ATOMIC_ACQUIRE),
        .midi_out_queue_drops =
            __atomic_load_n(&s_midi_out_queue_drops, __ATOMIC_ACQUIRE),
        .probe_event_drops =
            __atomic_load_n(&s_probe_event_drops, __ATOMIC_ACQUIRE),
        .recovery_requests =
            __atomic_load_n(&s_recovery_requests, __ATOMIC_ACQUIRE),
        .fault_recovery_epochs =
            controller_usb_recovery_gate_fault_epochs(
                &s_state.recovery_gate),
        .last_probe_result =
            __atomic_load_n(&s_last_probe_result, __ATOMIC_ACQUIRE),
        .last_seen_vid =
            __atomic_load_n(&s_last_seen_vid, __ATOMIC_ACQUIRE),
        .last_seen_pid =
            __atomic_load_n(&s_last_seen_pid, __ATOMIC_ACQUIRE),
        .last_config_total_length =
            __atomic_load_n(&s_last_config_total_length, __ATOMIC_ACQUIRE),
        .last_probe_stage =
            __atomic_load_n(&s_last_probe_stage, __ATOMIC_ACQUIRE),
        .last_probe_address =
            __atomic_load_n(&s_last_probe_address, __ATOMIC_ACQUIRE),
        .last_parent_port =
            __atomic_load_n(&s_last_parent_port, __ATOMIC_ACQUIRE),
        .last_direct_root =
            __atomic_load_n(&s_last_direct_root, __ATOMIC_ACQUIRE),
        .registered =
            __atomic_load_n(&s_registered, __ATOMIC_ACQUIRE),
        .connected = controller_usb_host_is_connected(),
        .accepting_midi_out =
            __atomic_load_n(&s_accepting_out, __ATOMIC_ACQUIRE),
    };
}

esp_err_t controller_usb_host_write_audio(const int16_t *master_samples,
                                          const int16_t *headphone_samples,
                                          size_t frame_count,
                                          uint32_t source_sample_rate)
{
    return controller_usb_audio_stream_write(
        master_samples, headphone_samples, frame_count, source_sample_rate);
}

bool controller_usb_host_audio_pace_ready(size_t frame_count,
                                         uint32_t source_sample_rate,
                                         bool *ready)
{
    return controller_usb_audio_stream_pace_ready(frame_count,
                                                  source_sample_rate, ready);
}

uint32_t controller_usb_host_audio_take_pace_low_water(void)
{
    return controller_usb_audio_stream_take_pace_low_water();
}

void controller_usb_host_get_audio_stats(
    controller_usb_host_audio_stats_t *stats_out)
{
    if (!stats_out) {
        return;
    }
    controller_usb_audio_stream_stats_t stream_stats = {0};
    controller_usb_audio_stream_get_stats(&stream_stats);
    *stats_out = (controller_usb_host_audio_stats_t) {
        .submitted_blocks = stream_stats.submitted_blocks,
        .dropped_blocks = stream_stats.dropped_blocks,
        .submitted_frames = stream_stats.submitted_frames,
        .ring_queued_frames = stream_stats.ring_queued_frames,
        .ring_capacity_frames = stream_stats.ring_capacity_frames,
        .ring_high_water_frames = stream_stats.ring_high_water_frames,
        .overrun_frames = stream_stats.overrun_frames,
        .underrun_frames = stream_stats.underrun_frames,
        .clock_trimmed_frames = stream_stats.clock_trimmed_frames,
        .clock_duplicated_frames = stream_stats.clock_duplicated_frames,
        .config_failures = stream_stats.config_failures,
        .transfer_failures = stream_stats.transfer_failures,
        .packet_failures = stream_stats.packet_failures,
        .packet_lost_frames = stream_stats.packet_lost_frames,
        .stream_epoch = stream_stats.stream_epoch,
        .claimed = stream_stats.claimed,
        .configuring = stream_stats.configuring,
        .streaming = stream_stats.streaming,
        .faulted = stream_stats.faulted,
    };
}
