/**
 * @file tusb_config.h
 * @brief TinyUSB host configuration for the JC1060 P4 deck.
 *
 * Host stack used for both DDJ-400 interfaces:
 *  - UAC1/2 audio host (speaker OUT, 4ch x 44.1 kHz x 24-bit)
 *  - MIDI host (bulk IN, jog/buttons parsing)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS               OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG            0

/* High-Speed root port (rhport 1 = OTG_HS / UTMI on ESP32-P4). */
#define CFG_TUH_ENABLED           1
#define CFG_TUH_MAX_SPEED         TUSB_SPEED_AUTO
#define CFG_TUH_ENUMERATION_BUFSIZE 512

#define CFG_TUH_HUB               0
#define CFG_TUH_CDC               0
#define CFG_TUH_HID               0
#define CFG_TUH_MSC               0
#define CFG_TUH_VENDOR            0

#define CFG_TUH_AUDIO             1
#define CFG_TUH_AUDIO_PROTOCOLS   (TUH_AUDIO_PROTOCOL_UAC1 | TUH_AUDIO_PROTOCOL_UAC2)
#define CFG_TUH_AUDIO_MAX         1
#define CFG_TUH_AUDIO_MAX_SAM_FREQ  8
#define CFG_TUH_AUDIO_MAX_AS        4
#define CFG_TUH_AUDIO_EPIN_BUFSIZE  512
#define CFG_TUH_AUDIO_EPOUT_BUFSIZE 1024
#define CFG_TUH_AUDIO_STREAM_BUFSIZE (48 * 1024)  /* ~27 ms at 4ch 24-bit 44.1k */

#define CFG_TUH_MIDI              1
#define CFG_TUH_MIDI_MAX          1

#define CFG_TUH_DEVICE_MAX        1
#define CFG_TUH_MEM_ALIGN         __attribute__((aligned(64)))

#ifdef __cplusplus
}
#endif
