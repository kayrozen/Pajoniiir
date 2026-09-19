/**
 * @file midi_host.h
 * @brief Minimal USB MIDI host (streaming interface claim + IN polling).
 */

#pragma once

#include <stdbool.h>
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise with the USB host client handle.
 */
bool midi_host_init(usb_host_client_handle_t client);

/**
 * @brief Attach to a device exposing a MIDI streaming interface.
 *
 * Claims the interface, locates the bulk IN endpoint and starts polling.
 * Parsed MIDI events (note on/off, CC, pitch bend) are logged.
 *
 * @param dev_hdl Open device handle
 * @param cfg_desc Active configuration descriptor of the device
 * @return true if the MIDI interface was claimed and polling started
 */
bool midi_host_attach(usb_device_handle_t dev_hdl, const usb_config_desc_t* cfg_desc);

/**
 * @brief Detach (device is going away).
 */
void midi_host_detach(usb_device_handle_t dev_hdl);

#ifdef __cplusplus
}
#endif
