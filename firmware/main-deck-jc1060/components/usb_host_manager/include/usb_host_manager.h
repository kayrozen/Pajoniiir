/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single owner for the ESP-IDF USB Host Library on ESP32-P4.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "usb/usb_host.h"
#include "usb_host_recovery_arbiter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_HOST_MANAGER_PERIPHERAL_MAP_DUAL ((1u << 0) | (1u << 1))

typedef struct {
    unsigned peripheral_map;
    bool root_port_unpowered;
    bool override_fs_phy_index;
    uint8_t fs_phy_index;
    int intr_flags;
    uint32_t daemon_stack_size;
    UBaseType_t daemon_priority;
    BaseType_t daemon_core_id;
} usb_host_manager_config_t;

typedef struct {
    esp_err_t install_result;
    uint32_t daemon_iterations;
    uint32_t daemon_errors;
    uint32_t no_clients_events;
    uint32_t all_free_events;
    uint32_t root_power_requested_mask;
    uint32_t topology_observations;
    uint32_t topology_probe_failures;
    int32_t last_topology_result;
    uint8_t last_topology_address;
    uint8_t last_topology_parent_port;
    bool last_topology_direct_root;
    uint32_t recovery_queue_drops;
    uint32_t recovery_requests;
    uint32_t recovery_coalesced_requests;
    uint32_t recovery_successes;
    uint32_t recovery_suppressed_active;
    uint32_t recovery_failures;
    uint32_t host_restarts;
    uint32_t host_restart_failures;
    uint32_t host_generation;
    unsigned peripheral_map;
    bool fs_phy_override_requested;
    uint8_t fs_phy_index;
    bool ready;
    bool root_power_requested;
} usb_host_manager_diagnostics_t;

esp_err_t usb_host_manager_init(const usb_host_manager_config_t *config);
bool usb_host_manager_is_ready(void);
esp_err_t usb_host_manager_set_all_root_power(bool enable);
esp_err_t usb_host_manager_set_root_power_by_index(uint8_t root_port_index,
                                                   bool enable);
/* jc1060 board extension (v169): observed virtual-root port of a device.
 * The fork's root hub merges both DWCs and the external hub ports, so a
 * stick behind a hub reports root=2..N. */
esp_err_t usb_host_manager_device_root(uint8_t address,
                                       uint8_t *root_port_index_out,
                                       bool *known_out);

esp_err_t usb_host_manager_device_matches_root(uint8_t address,
                                               uint8_t root_port_index,
                                               bool require_direct_root,
                                               bool *matches_out);
esp_err_t usb_host_manager_request_recovery(
    uint8_t root_port_index,
    usb_host_recovery_reason_t reason);
esp_err_t usb_host_manager_get_library_info(usb_host_lib_info_t *info_out);
void usb_host_manager_get_diagnostics(usb_host_manager_diagnostics_t *diag_out);

/* v238: full Host Library restart (usb_host_uninstall + usb_host_install
 * with the boot config, which also re-creates the virtual root hub). The
 * forced root power-cycle produces no connection event on this fork, so a
 * wedged device can only be re-enumerated this way.
 *
 * Every task owning a Host Library client registers once as a participant.
 * While usb_host_manager_host_restart_pending() is true it closes its
 * devices, deregisters its client, calls ..._host_restart_release() with the
 * generation it saw, then waits in ..._wait_host_restart() and registers a
 * new client. The daemon waits a bounded time for every participant; it
 * cannot uninstall while a client is still registered and keeps the old
 * stack in that case. Both roots come back unpowered (root_port_unpowered):
 * each participant re-powers its own root after the restart. */
#define USB_HOST_MANAGER_RESTART_PARTICIPANTS_MAX 4u

esp_err_t usb_host_manager_restart_participant_register(const char *name,
                                                        uint32_t *id_out);
esp_err_t usb_host_manager_request_host_restart(const char *why);
bool usb_host_manager_host_restart_pending(void);
uint32_t usb_host_manager_host_generation(void);
void usb_host_manager_host_restart_release(uint32_t id, uint32_t generation);
/* ESP_OK once the generation moved past `generation` with a ready stack,
 * ESP_ERR_INVALID_STATE if the reinstall failed, ESP_ERR_TIMEOUT. */
esp_err_t usb_host_manager_wait_host_restart(uint32_t generation,
                                             TickType_t timeout);

#ifdef __cplusplus
}
#endif
