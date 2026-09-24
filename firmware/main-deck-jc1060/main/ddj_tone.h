/**
 * @file ddj_tone.h
 * @brief DDJ-400 UAC bring-up tone (master + headphones).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Start the tone producer task (idempotent-safe to call once at boot). */
void ddj_tone_start(void);

#ifdef __cplusplus
}
#endif
