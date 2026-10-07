#pragma once
//
// dj_link_mixer (v341) - what a DJ Link mixer (DJM) tells the players: fader
// start (port 50001, type 0x02) and channels on air (type 0x03).
//
// Fader start: the mixer sends one command per player number 1..4
// (START / STOP / IGNORE) when a channel fader leaves or reaches the bottom.
// A player acts on the change, not on every packet: a mixer may repeat a
// command, and a deck the operator paused by hand must not restart because the
// same START arrived again. Channels on air: the mixer says which channels
// are audible; a player shows it in its status (flag 0x08).
//
// Pure: no clock of its own, no heap, host tested. The sender must be a mixer
// (device number 33 and up): a player never sends these.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_MIXER_CHANNELS     4u      /* the packets carry 4 */
#define DJ_LINK_MIXER_FIRST_NUMBER 0x21u   /* mixer device numbers */
#define DJ_LINK_ON_AIR_HOLD_MS     5000u   /* on air this long after the last packet */

typedef enum {
    DJ_LINK_FADER_NONE = 0,   /* nothing new */
    DJ_LINK_FADER_START,
    DJ_LINK_FADER_STOP,
} dj_link_fader_event_t;

typedef struct {
    uint8_t  fader_last[DJ_LINK_MIXER_CHANNELS];     /* last command seen, 0xff = none yet */
    uint8_t  fader_pending[DJ_LINK_MIXER_CHANNELS];  /* dj_link_fader_event_t not taken yet */
    uint8_t  on_air[DJ_LINK_MIXER_CHANNELS];
    uint32_t on_air_rx_ms[DJ_LINK_MIXER_CHANNELS];
    bool     on_air_seen[DJ_LINK_MIXER_CHANNELS];
    uint32_t accepted;                               /* packets used */
    uint32_t rejected;                               /* mixer-looking packets refused */
} dj_link_mixer_t;

void dj_link_mixer_reset(dj_link_mixer_t *m);

/* One datagram of port 50001. True if it was a fader start or on-air packet
 * from a mixer and was used; false for anything else (the caller goes on to
 * the generic ingest). Fader commands other than START / STOP / IGNORE and
 * packets from a player number are refused. */
bool dj_link_mixer_ingest(dj_link_mixer_t *m, const uint8_t *buf, size_t len, uint32_t now_ms);

/* The change for player `number` (1..4) since the last call, once. */
dj_link_fader_event_t dj_link_mixer_fader_take(dj_link_mixer_t *m, uint8_t number);

/* Player `number` is on air: the mixer flagged its channel and spoke within
 * DJ_LINK_ON_AIR_HOLD_MS. False without any packet. */
bool dj_link_mixer_on_air(const dj_link_mixer_t *m, uint8_t number, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
