/**
 * @file usb_host_bringup.h
 * @brief USB host bring-up helper (enumeration + MIDI/MSC/hub detection)
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the USB host bring-up tasks (daemon + enumerator).
 *
 * Non-fatal: logs every attached device (VID/PID, interfaces, class).
 * Intended for the HS USB-C port with a powered hub.
 *
 * @return true if the host library installed successfully
 */
bool usb_host_bringup_start(void);

#ifdef __cplusplus
}
#endif
