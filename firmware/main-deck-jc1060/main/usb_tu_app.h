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

#ifdef __cplusplus
}
#endif
