#include "dj_link_mixer.h"

#include <string.h>

#include "djlink/mixer.h"

#define FADER_NONE_YET 0xffu

void dj_link_mixer_reset(dj_link_mixer_t *m)
{
    memset(m, 0, sizeof(*m));
    memset(m->fader_last, FADER_NONE_YET, sizeof(m->fader_last));
}

static bool from_mixer(const uint8_t *buf)
{
    return buf[0x21] >= DJ_LINK_MIXER_FIRST_NUMBER;
}

bool dj_link_mixer_ingest(dj_link_mixer_t *m, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (!m || !buf || len <= 0x21u || buf[0x0a] < 0x02u || buf[0x0a] > 0x03u) {
        return false;
    }
    const uint8_t type = buf[0x0a];
    if (type == 0x02u) {
        djlink_fader_t f;
        if (djlink_fader_parse(buf, len, &f) != DJLINK_OK) {
            return false;
        }
        if (!from_mixer(buf)) {
            m->rejected++;
            return true;
        }
        for (uint32_t i = 0; i < DJ_LINK_MIXER_CHANNELS; i++) {
            const uint8_t cmd = (uint8_t)f.players[i];
            if (cmd != DJLINK_FADER_START && cmd != DJLINK_FADER_STOP &&
                cmd != DJLINK_FADER_IGNORE) {
                m->rejected++;
                continue;
            }
            if (cmd != m->fader_last[i]) {
                if (cmd == DJLINK_FADER_START) {
                    m->fader_pending[i] = DJ_LINK_FADER_START;
                } else if (cmd == DJLINK_FADER_STOP) {
                    m->fader_pending[i] = DJ_LINK_FADER_STOP;
                } else {
                    m->fader_pending[i] = DJ_LINK_FADER_NONE;   /* IGNORE: let go */
                }
                m->fader_last[i] = cmd;
            }
        }
        m->accepted++;
        return true;
    }
    djlink_onair_t a;
    if (djlink_onair_parse(buf, len, &a) != DJLINK_OK) {
        return false;
    }
    if (!from_mixer(buf)) {
        m->rejected++;
        return true;
    }
    for (uint32_t i = 0; i < DJ_LINK_MIXER_CHANNELS; i++) {
        m->on_air[i] = a.on_air[i] != 0u;
        m->on_air_rx_ms[i] = now_ms;
        m->on_air_seen[i] = true;
    }
    m->accepted++;
    return true;
}

dj_link_fader_event_t dj_link_mixer_fader_take(dj_link_mixer_t *m, uint8_t number)
{
    if (!m || number < 1u || number > DJ_LINK_MIXER_CHANNELS) {
        return DJ_LINK_FADER_NONE;
    }
    const dj_link_fader_event_t e = (dj_link_fader_event_t)m->fader_pending[number - 1u];
    m->fader_pending[number - 1u] = DJ_LINK_FADER_NONE;
    return e;
}

bool dj_link_mixer_on_air(const dj_link_mixer_t *m, uint8_t number, uint32_t now_ms)
{
    if (!m || number < 1u || number > DJ_LINK_MIXER_CHANNELS || !m->on_air_seen[number - 1u]) {
        return false;
    }
    return m->on_air[number - 1u] != 0u &&
           (uint32_t)(now_ms - m->on_air_rx_ms[number - 1u]) < DJ_LINK_ON_AIR_HOLD_MS;
}
