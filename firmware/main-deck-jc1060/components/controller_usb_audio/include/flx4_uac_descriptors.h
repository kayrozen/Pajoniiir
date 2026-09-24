#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t interface_num;
    uint8_t alternate_setting;
    uint8_t endpoint_addr;
    uint16_t max_packet_size;
    /* OUT endpoint bmAttributes (bits 3:2 = sync type: 1 async, 2 adaptive,
     * 3 sync), UAC1 bSynchAddress and any IN isochronous endpoint in the
     * same alt (explicit feedback candidate). Diagnostic only: the stream
     * does not service feedback. */
    uint8_t endpoint_attributes;
    uint8_t sync_address;
    uint8_t in_endpoint_addr;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t bytes_per_sample;
    uint32_t sample_rates[8];
    uint8_t sample_rate_count;
    /* bSamFreqType == 0: sample_rates[0..1] hold tLowerSamFreq and
     * tUpperSamFreq of a continuous range instead of discrete rates. */
    bool sample_rate_continuous;
} flx4_uac_playback_format_t;

typedef struct {
    flx4_uac_playback_format_t formats[8];
    uint8_t format_count;
} flx4_uac_descriptor_result_t;

bool flx4_uac_parse_playback_formats(const uint8_t *config_desc,
                                     size_t config_len,
                                     flx4_uac_descriptor_result_t *out);

bool flx4_uac_select_preferred_format(const flx4_uac_descriptor_result_t *result,
                                      flx4_uac_playback_format_t *out);

#ifdef __cplusplus
}
#endif
