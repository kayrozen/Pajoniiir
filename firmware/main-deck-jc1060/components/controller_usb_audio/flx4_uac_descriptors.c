#include "flx4_uac_descriptors.h"

#include <string.h>

#define USB_DESC_TYPE_CONFIG       0x02u
#define USB_DESC_TYPE_INTERFACE    0x04u
#define USB_DESC_TYPE_ENDPOINT     0x05u
#define USB_DESC_TYPE_CS_INTERFACE 0x24u

#define USB_CLASS_AUDIO              0x01u
#define USB_SUBCLASS_AUDIOSTREAMING  0x02u

#define UAC_AS_FORMAT_TYPE 0x02u
#define UAC_FORMAT_TYPE_I  0x01u

#define USB_EP_DIR_IN    0x80u
#define USB_EP_XFER_MASK 0x03u
#define USB_EP_XFER_ISOC 0x01u

#define FLX4_UAC_MAX_FORMATS 8u
#define FLX4_UAC_MAX_RATES   8u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static bool add_rate(flx4_uac_playback_format_t *fmt, uint32_t rate)
{
    if (!fmt || rate == 0u || fmt->sample_rate_count >= FLX4_UAC_MAX_RATES) {
        return false;
    }
    fmt->sample_rates[fmt->sample_rate_count++] = rate;
    return true;
}

static bool format_has_rate(const flx4_uac_playback_format_t *fmt, uint32_t rate)
{
    if (!fmt) {
        return false;
    }
    if (fmt->sample_rate_continuous) {
        return fmt->sample_rate_count == 2u &&
               rate >= fmt->sample_rates[0] && rate <= fmt->sample_rates[1];
    }
    for (uint8_t i = 0; i < fmt->sample_rate_count; ++i) {
        if (fmt->sample_rates[i] == rate) {
            return true;
        }
    }
    return false;
}

static uint32_t packet_bytes_for_rate(const flx4_uac_playback_format_t *fmt,
                                      uint32_t rate)
{
    if (!fmt || rate == 0u) {
        return UINT32_MAX;
    }
    const uint32_t frames_per_ms = (rate + 999u) / 1000u;
    return frames_per_ms * (uint32_t)fmt->channels * (uint32_t)fmt->bytes_per_sample;
}

static bool format_has_supported_packetization(const flx4_uac_playback_format_t *fmt)
{
    if (!fmt ||
        !((fmt->bits_per_sample == 16u && fmt->bytes_per_sample == 2u) ||
          (fmt->bits_per_sample == 24u && fmt->bytes_per_sample == 3u)) ||
        (fmt->channels != 2u && fmt->channels != 4u)) {
        return false;
    }
    static const uint32_t rates[] = { 44100u, 48000u };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        const uint32_t rate = rates[i];
        if (format_has_rate(fmt, rate) &&
            packet_bytes_for_rate(fmt, rate) <= fmt->max_packet_size) {
            return true;
        }
    }
    return false;
}

static bool is_complete_playback_format(const flx4_uac_playback_format_t *fmt)
{
    return fmt &&
           fmt->endpoint_addr != 0u &&
           (fmt->endpoint_addr & USB_EP_DIR_IN) == 0u &&
           fmt->max_packet_size != 0u &&
           fmt->channels != 0u &&
           fmt->bits_per_sample != 0u &&
           fmt->bytes_per_sample != 0u &&
           fmt->sample_rate_count != 0u;
}

static void append_current_format(flx4_uac_descriptor_result_t *out,
                                  const flx4_uac_playback_format_t *current)
{
    if (!out || out->format_count >= FLX4_UAC_MAX_FORMATS || !is_complete_playback_format(current)) {
        return;
    }
    out->formats[out->format_count++] = *current;
}

bool flx4_uac_parse_playback_formats(const uint8_t *config_desc,
                                     size_t config_len,
                                     flx4_uac_descriptor_result_t *out)
{
    if (!config_desc || !out || config_len < 9u || config_desc[1] != USB_DESC_TYPE_CONFIG) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    size_t total_len = (size_t)rd16(&config_desc[2]);
    if (total_len == 0u || total_len > config_len) {
        total_len = config_len;
    }

    flx4_uac_playback_format_t current = { 0 };
    bool in_audio_streaming_alt = false;

    for (size_t off = config_desc[0]; off + 2u <= total_len;) {
        const uint8_t len = config_desc[off];
        const uint8_t type = config_desc[off + 1u];
        if (len < 2u || off + len > total_len) {
            memset(out, 0, sizeof(*out));
            return false;
        }

        if (type == USB_DESC_TYPE_INTERFACE && len >= 9u) {
            append_current_format(out, &current);
            memset(&current, 0, sizeof(current));

            in_audio_streaming_alt =
                config_desc[off + 5u] == USB_CLASS_AUDIO &&
                config_desc[off + 6u] == USB_SUBCLASS_AUDIOSTREAMING &&
                config_desc[off + 3u] != 0u;

            if (in_audio_streaming_alt) {
                current.interface_num = config_desc[off + 2u];
                current.alternate_setting = config_desc[off + 3u];
            }
        } else if (in_audio_streaming_alt && type == USB_DESC_TYPE_CS_INTERFACE && len >= 8u) {
            /* UAC1 Type I format descriptor (Frmts 2.2.5): bFormatType @3,
             * bNrChannels @4, bSubframeSize @5, bBitResolution @6,
             * bSamFreqType @7, rates @8. Only the first Type I descriptor of
             * an alt setting counts; Type II/III use a different layout and
             * a malformed width leaves the alt incomplete (never streamed)
             * rather than guessed. */
            const uint8_t subtype = config_desc[off + 2u];
            if (subtype == UAC_AS_FORMAT_TYPE &&
                config_desc[off + 3u] == UAC_FORMAT_TYPE_I &&
                current.channels == 0u) {
                const uint8_t subframe = config_desc[off + 5u];
                const uint8_t resolution = config_desc[off + 6u];
                if (subframe >= 1u && subframe <= 4u && resolution != 0u &&
                    resolution <= (uint8_t)(subframe * 8u)) {
                    current.channels = config_desc[off + 4u];
                    current.bytes_per_sample = subframe;
                    current.bits_per_sample = resolution;
                }

                const uint8_t rate_count = config_desc[off + 7u];
                if (rate_count == 0u && len >= 14u) {
                    current.sample_rate_continuous = true;
                    (void)add_rate(&current, rd24(&config_desc[off + 8u]));
                    (void)add_rate(&current, rd24(&config_desc[off + 11u]));
                } else if (rate_count != 0u &&
                           len >= 8u + (size_t)rate_count * 3u) {
                    for (uint8_t i = 0; i < rate_count; ++i) {
                        (void)add_rate(&current, rd24(&config_desc[off + 8u + ((size_t)i * 3u)]));
                    }
                }
            }
        } else if (in_audio_streaming_alt && type == USB_DESC_TYPE_ENDPOINT && len >= 7u) {
            const uint8_t ep = config_desc[off + 2u];
            const uint8_t transfer_type = config_desc[off + 3u] & USB_EP_XFER_MASK;
            if ((ep & USB_EP_DIR_IN) == 0u && transfer_type == USB_EP_XFER_ISOC) {
                current.endpoint_addr = ep;
                current.max_packet_size = rd16(&config_desc[off + 4u]);
                current.endpoint_attributes = config_desc[off + 3u];
                current.sync_address = len >= 9u ? config_desc[off + 8u] : 0u;
            } else if ((ep & USB_EP_DIR_IN) != 0u &&
                       transfer_type == USB_EP_XFER_ISOC) {
                current.in_endpoint_addr = ep;
            }
        }

        off += len;
    }

    append_current_format(out, &current);
    return out->format_count > 0u;
}

bool flx4_uac_select_preferred_format(const flx4_uac_descriptor_result_t *result,
                                      flx4_uac_playback_format_t *out)
{
    if (!result || !out) {
        return false;
    }

    const flx4_uac_playback_format_t *best = NULL;
    int best_score = -1;

    for (uint8_t i = 0; i < result->format_count && i < FLX4_UAC_MAX_FORMATS; ++i) {
        const flx4_uac_playback_format_t *fmt = &result->formats[i];
        if (!is_complete_playback_format(fmt) ||
            !format_has_supported_packetization(fmt)) {
            continue;
        }

        int score = 0;
        if (format_has_rate(fmt, 48000u)) {
            score += 1000;
        }
        if (format_has_rate(fmt, 44100u)) {
            score += 500;
        }
        /* Unsupported sample widths never reach scoring. */
        score += 200;
        if (fmt->channels == 4u) {
            score += 40;
        } else if (fmt->channels == 2u) {
            score += 20;
        }
        score += (int)fmt->alternate_setting;

        if (score > best_score) {
            best_score = score;
            best = fmt;
        }
    }

    if (!best) {
        return false;
    }

    *out = *best;
    return true;
}
