/* SPDX-License-Identifier: Apache-2.0 */
#include "controller_led_runtime.h"

#include "controller_profile.h"
#include "controller_profile_runtime.h"
#include "controller_usb_host.h"
#include "control_link.h"
#include "flx4_led_midi.h"
#include "esp_log.h"

#include <string.h>

/* v220 diagnostic: the first LED requests are logged with their outcome
 * (MIDI bytes, no mapping, or send error) to tell a missing profile LED map
 * from a USB-side loss. */
#define LED_LOG_LIMIT 24u
/* v222: one window per deck (0, 1, other) and the continuous VU meter traced
 * only twice per deck, so deck 1 lines are not crowded out by deck 2 or VU. */
#define LED_LOG_SLOTS 3u
#define LED_VU_LOG_LIMIT 2u
static const char *TAG = "controller_led";
static uint32_t s_led_logged[LED_LOG_SLOTS];
static uint32_t s_led_vu_logged[LED_LOG_SLOTS];
/* v223: an unmapped LED id is logged once per id instead of spending the deck
 * window. DDJ-400: LED_SMART_CFX (41) and LED_SMART_FADER (42) have no
 * hardware (FLX4-only, note 0x96 00/01) and are sent on deck 0 only. */
static uint32_t s_led_unmapped_logged[(LED_REMOTE_COUNT + 31) / 32];

/* v226: the per-controller VU curve (DDJ-400 * 150 / 127, v224) is now the
 * profile's cc_value "scale", applied in cp_profile_map_led(). */
static bool s_led_profile_seen;

static uint32_t s_dynamic_packets;
static uint32_t s_builtin_packets;
static uint32_t s_builtin_fallbacks;
static uint32_t s_unsupported;
static uint32_t s_send_failures;
static bool s_builtin_flx4_enabled;

void controller_led_runtime_set_builtin_flx4_enabled(bool enabled)
{
    __atomic_store_n(&s_builtin_flx4_enabled, enabled, __ATOMIC_RELEASE);
    /* Called on every connect/disconnect: re-arm the v220 LED trace. */
    memset(s_led_logged, 0, sizeof(s_led_logged));
    memset(s_led_vu_logged, 0, sizeof(s_led_vu_logged));
    memset(s_led_unmapped_logged, 0, sizeof(s_led_unmapped_logged));
}

bool controller_led_runtime_build_packet(uint8_t led,
                                         uint8_t state,
                                         uint8_t deck,
                                         uint8_t packet[4])
{
    if (!packet) {
        return false;
    }

    const bool profile_active = controller_profile_runtime_active();
    const bool builtin_enabled =
        __atomic_load_n(&s_builtin_flx4_enabled, __ATOMIC_ACQUIRE);
    const bool authoritative = builtin_enabled &&
        flx4_led_midi_builtin_authoritative(led);
    if (profile_active && !authoritative &&
        controller_profile_runtime_map_led(led, deck, state, packet)) {
        (void)__atomic_add_fetch(&s_dynamic_packets, 1u,
                                 __ATOMIC_RELAXED);
        return true;
    }

    if (builtin_enabled &&
        flx4_led_midi_build_packet(led, state, deck, packet)) {
        (void)__atomic_add_fetch(&s_builtin_packets, 1u,
                                 __ATOMIC_RELAXED);
        if (profile_active && !authoritative) {
            (void)__atomic_add_fetch(&s_builtin_fallbacks, 1u,
                                     __ATOMIC_RELAXED);
        }
        return true;
    }

    (void)__atomic_add_fetch(&s_unsupported, 1u, __ATOMIC_RELAXED);
    return false;
}

esp_err_t controller_led_runtime_send_profile_init(void)
{
    uint8_t packets[(CP_MAX_INIT_SYSEX + 2) / 3][4];
    const size_t count = controller_profile_runtime_init_sysex_packets(
        packets, sizeof(packets) / sizeof(packets[0]));
    if (count == 0u) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t rc = ESP_OK;
    size_t sent = 0u;
    for (; sent < count && rc == ESP_OK; ++sent) {
        rc = controller_usb_host_send_packet(packets[sent]);
    }
    if (rc != ESP_OK) {
        (void)__atomic_add_fetch(&s_send_failures, 1u, __ATOMIC_RELAXED);
    }
    ESP_LOGW(TAG, "profile init SysEx: %u USB-MIDI packets queued (%u sent) "
                  "rc=%s", (unsigned)count,
             (unsigned)(rc == ESP_OK ? sent : sent - 1u), esp_err_to_name(rc));
    return rc;
}

esp_err_t controller_led_runtime_send(uint8_t led,
                                      uint8_t state,
                                      uint8_t deck)
{
    uint8_t packet[4];
    /* v221: the profile activates asynchronously after connect, so the
     * connect-time window can be spent before it. Re-arm the trace on the
     * first request that sees the profile active. */
    const bool profile_now = controller_profile_runtime_active();
    if (profile_now != s_led_profile_seen) {
        s_led_profile_seen = profile_now;
        memset(s_led_logged, 0, sizeof(s_led_logged));
        memset(s_led_vu_logged, 0, sizeof(s_led_vu_logged));
        memset(s_led_unmapped_logged, 0, sizeof(s_led_unmapped_logged));
        ESP_LOGW(TAG, "LED path sees profile_active=%d", profile_now ? 1 : 0);
    }
    if (!controller_led_runtime_build_packet(led, state, deck, packet)) {
        const uint32_t bit = 1u << (led % 32u);
        if (led < LED_REMOTE_COUNT &&
            !(s_led_unmapped_logged[led / 32u] & bit)) {
            s_led_unmapped_logged[led / 32u] |= bit;
            ESP_LOGW(TAG, "LED %u deck %u state %u -> no mapping "
                          "(profile_active=%d builtin=%d, logged once)",
                     led, deck, state,
                     controller_profile_runtime_active() ? 1 : 0,
                     __atomic_load_n(&s_builtin_flx4_enabled,
                                     __ATOMIC_ACQUIRE) ? 1 : 0);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    const unsigned slot = deck < LED_LOG_SLOTS - 1u ? deck : LED_LOG_SLOTS - 1u;
    bool trace;
    if (led == LED_VU_METER) {
        trace = s_led_vu_logged[slot] < LED_VU_LOG_LIMIT;
        if (trace) {
            s_led_vu_logged[slot]++;
        }
    } else {
        trace = s_led_logged[slot] < LED_LOG_LIMIT;
        if (trace) {
            s_led_logged[slot]++;
        }
    }
    const esp_err_t rc = controller_usb_host_send_packet(packet);
    if (trace) {
        ESP_LOGW(TAG, "LED %u deck %u state %u -> %02X %02X %02X %02X rc=%s",
                 led, deck, state, packet[0], packet[1], packet[2],
                 packet[3], esp_err_to_name(rc));
    }
    if (rc != ESP_OK) {
        (void)__atomic_add_fetch(&s_send_failures, 1u,
                                 __ATOMIC_RELAXED);
        return rc;
    }

    controller_usb_identity_t identity;
    if (controller_usb_host_get_identity(&identity) &&
        identity.vid == 0x2B73u && identity.pid == 0x0045u &&
        flx4_led_midi_build_shifted_mirror_packet(
            led, state, deck, packet)) {
        const esp_err_t mirror_rc = controller_usb_host_send_packet(packet);
        if (mirror_rc != ESP_OK) {
            (void)__atomic_add_fetch(&s_send_failures, 1u,
                                     __ATOMIC_RELAXED);
            return mirror_rc;
        }
    }
    return ESP_OK;
}

void controller_led_runtime_get_diagnostics(
    controller_led_runtime_diagnostics_t *diag_out)
{
    if (!diag_out) {
        return;
    }
    *diag_out = (controller_led_runtime_diagnostics_t) {
        .dynamic_packets =
            __atomic_load_n(&s_dynamic_packets, __ATOMIC_ACQUIRE),
        .builtin_packets =
            __atomic_load_n(&s_builtin_packets, __ATOMIC_ACQUIRE),
        .builtin_fallbacks =
            __atomic_load_n(&s_builtin_fallbacks, __ATOMIC_ACQUIRE),
        .unsupported =
            __atomic_load_n(&s_unsupported, __ATOMIC_ACQUIRE),
        .send_failures =
            __atomic_load_n(&s_send_failures, __ATOMIC_ACQUIRE),
    };
}
