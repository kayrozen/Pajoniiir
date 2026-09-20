/**
 * @file usb_tu_app.h
 * @brief TinyUSB host bring-up for the DDJ-400 (UAC1 audio + MIDI).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start TinyUSB host on the P4 High-Speed root port. */
esp_err_t usb_tu_start(void);

/** True while the audio path owns the radio (Wi-Fi/SDIO suspended so the
 * USB isochronous stream keeps its bus time). Wi-Fi reconnect logic must
 * stand down while this is set. */
bool usb_audio_wifi_suspended(void);

#ifdef __cplusplus
}
#endif
