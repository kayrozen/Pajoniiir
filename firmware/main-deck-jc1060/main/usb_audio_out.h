/**
 * @file usb_audio_out.h
 * @brief Minimal USB Audio Class host OUT (DDJ-400 master + phones).
 */

#pragma once

#include <stdbool.h>
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Claim the audio streaming OUT interface and start the isochronous pipe. */
bool usb_audio_out_attach(usb_host_client_handle_t client,
                          usb_device_handle_t dev_hdl,
                          const usb_config_desc_t* cfg_desc);

/** Stop the pipe and release (device going away). */
void usb_audio_out_detach(usb_device_handle_t dev_hdl);

#ifdef __cplusplus
}
#endif
