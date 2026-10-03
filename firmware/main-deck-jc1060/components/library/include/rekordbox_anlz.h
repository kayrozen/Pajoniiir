#pragma once
/*
 * rekordbox_anlz.h  —  Rekordbox ANLZ file parser
 *
 * Parses ANLZ0000.DAT + ANLZ0000.EXT from a Rekordbox-formatted USB drive.
 *
 * USB drive layout (HASH-BASED — NOT a path mirror of the audio file):
 *   PIONEER/USBANLZ/<P-hash>/<ID-hash>/ANLZ0000.DAT  — path, BPM, beatgrid, cues, waveform
 *   PIONEER/USBANLZ/<P-hash>/<ID-hash>/ANLZ0000.EXT  — high-res waveform (PWV3)
 *   PIONEER/USBANLZ/<P-hash>/<ID-hash>/ANLZ0000.2EX  — color waveform (newer Rekordbox)
 *
 * All multi-byte values in ANLZ files are big-endian.
 * Tags are located by walking the section headers (tag/header_size/segment_size);
 * a byte-scan fallback handles structurally broken files.
 *
 * Usage:
 *   anlz_metadata_t meta;
 *   esp_err_t rc = anlz_parse_dat("/usb/PIONEER/USBANLZ/P000/00000832/ANLZ0000.DAT", &meta);
 *   if (rc == ESP_OK) {
 *       anlz_parse_ext("/usb/PIONEER/USBANLZ/artist/track/ANLZ0000.EXT", &meta);  // optional
 *       // use meta.bpm, meta.beats, meta.cues, meta.waveform_low, meta.waveform_high ...
 *       anlz_free(&meta);
 *   }
 *
 * Compile-time option (PC test build only):
 *   #define ANLZ_STANDALONE_TEST   — replaces ESP_LOG with printf, removes esp_err.h dependency
 */

#ifdef ANLZ_STANDALONE_TEST
#  include <stdio.h>
#  define ESP_OK          0
#  define ESP_ERR_INVALID_ARG  1
#  define ESP_ERR_INVALID_SIZE 2
#  define ESP_ERR_NOT_FOUND    3
#  define ESP_ERR_NO_MEM       4
#  define ESP_FAIL             5
typedef int esp_err_t;
#  define ANLZ_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#  define ANLZ_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#  define ANLZ_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#  include "esp_err.h"
#  include "esp_log.h"
#  define ANLZ_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#  define ANLZ_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#  define ANLZ_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tag IDs (4 ASCII bytes, stored as uint32_t big-endian) ───────────────── */
#define ANLZ_TAG_PMAI  0x504D4149u  /* 'PMAI' — file header            */
#define ANLZ_TAG_PPTH  0x50505448u  /* 'PPTH' — audio file path        */
#define ANLZ_TAG_PVBR  0x50564252u  /* 'PVBR' — VBR seek table         */
#define ANLZ_TAG_PQTZ  0x5051545Au  /* 'PQTZ' — beat grid              */
#define ANLZ_TAG_PWAV  0x50574156u  /* 'PWAV' — waveform low-res       */
#define ANLZ_TAG_PWV2  0x50575632u  /* 'PWV2' — waveform tiny          */
#define ANLZ_TAG_PCOB  0x50434F42u  /* 'PCOB' — cue objects container  */
#define ANLZ_TAG_PWV3  0x50575633u  /* 'PWV3' — waveform high-res      */
#define ANLZ_TAG_PWV4  0x50575634u  /* 'PWV4' — colour preview (v313)  */

/* ── Sizes ────────────────────────────────────────────────────────────────── */
#define ANLZ_WAVEFORM_LOW_LEN    400u   /* PWAV: always 400 bytes          */
#define ANLZ_WAVEFORM_TINY_LEN   100u   /* PWV2: always 100 nibble entries */
#define ANLZ_VBR_TABLE_LEN       400u   /* PVBR: 400 × uint32_t offsets    */
#define ANLZ_MAX_CUES              8u   /* hot cues 0–7                    */
#define ANLZ_MAX_MEMORY_CUES      16u   /* v309: earliest memory cues kept */
/* v313: PWV4 colour preview in .EXT: 1200 entries of 6 bytes. */
#define ANLZ_COLOR_PREVIEW_ENTRY    6u
#define ANLZ_COLOR_PREVIEW_MAX   7200u  /* bytes: 1200 entries */
#define ANLZ_WAVEFORM_HIGH_MAX 131072u  /* PWV3: up to 128 KB (observed max ~62 KB) */
#define ANLZ_PATH_MAX            512u   /* audio path buffer               */
#define ANLZ_WAVEFORM_HIGH_PER_S 150u   /* PWV3: entries per second of audio */
/* PWV3 may be at most this far from the PDB length (whole seconds) to be
 * taken as the track length; beyond it (truncated PWV3, other file) PDB wins. */
#define ANLZ_DURATION_PWV3_TOLERANCE_MS 1500u

/* v270: the track length used as the time base of the PVBR seek table, the
 * waveform and the touch mapping. The PDB stores whole seconds, up to ~1 s
 * off; a PVBR entry lands proportionally off (v269 HW: mini-waveform seeks
 * desynced from the zoom). PWV3 has 150 entries per second of audio, so its
 * length is the track length to 1/150 s. 0 = no usable length. */
static inline uint32_t anlz_precise_duration_ms(uint32_t pdb_ms, uint32_t pwv3_len)
{
    if (pwv3_len == 0u || pwv3_len >= ANLZ_WAVEFORM_HIGH_MAX) return pdb_ms;
    const uint32_t pwv3_ms =
        (uint32_t)(((uint64_t)pwv3_len * 1000u) / ANLZ_WAVEFORM_HIGH_PER_S);
    if (pdb_ms == 0u) return pwv3_ms;
    const uint32_t diff = pwv3_ms > pdb_ms ? pwv3_ms - pdb_ms : pdb_ms - pwv3_ms;
    return diff <= ANLZ_DURATION_PWV3_TOLERANCE_MS ? pwv3_ms : pdb_ms;
}

/* ── Beat grid entry (8 bytes, big-endian in file) ────────────────────────── */
typedef struct {
    uint16_t beat_phase;   /* PQTZ beat number in the bar: 1..4, 1 = downbeat
                            * (rekordbox, dbserver beatgrid, vynull); 0 = unknown */
    uint16_t bpm_x100;     /* BPM × 100  (e.g. 12850 → 128.50 BPM)       */
    uint32_t time_ms;      /* absolute position from start of track (ms)  */
} anlz_beat_t;

/* v306: beat_phase is the 1-based beat number, not a 0-based phase. Reading
 * `beat_phase % 4 == 0` as the downbeat put it on beat 4, one beat early. */
static inline bool anlz_beat_is_downbeat(uint16_t beat_phase)
{
    return beat_phase == 1u;
}

/* 0..3 position in the bar (0 = downbeat); 0 when the number is unknown. */
static inline uint8_t anlz_beat_bar_index(uint16_t beat_phase)
{
    return beat_phase >= 1u && beat_phase <= 4u ? (uint8_t)(beat_phase - 1u) : 0u;
}

/* ── Cue type ─────────────────────────────────────────────────────────────── */
typedef enum {
    ANLZ_CUE_SINGLE = 1,   /* single hot cue point    */
    ANLZ_CUE_LOOP   = 2,   /* loop (start + end)      */
} anlz_cue_type_t;

/* ── Single cue / loop entry ─────────────────────────────────────────────── */
typedef struct {
    anlz_cue_type_t type;   /* single point or loop                        */
    uint8_t         index;  /* hot cue slot 0–7                            */
    uint32_t        start_ms;
    uint32_t        end_ms; /* loop end; 0 for single cues                 */
} anlz_cue_t;

/* v309: one memory cue (type-0 PCOB entry): a point, or a loop when end_ms
 * is above start_ms (0 for a point). */
typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
} anlz_memory_cue_t;

/* ── Parsed metadata for one track ──────────────────────────────────────────
 *
 * Heap allocations:
 *   beats         — heap-allocated array of beat_count entries, or NULL
 *   waveform_high — heap-allocated array of waveform_high_len bytes, or NULL
 *
 * All other fields are inline.  Call anlz_free() when done.
 */
typedef struct anlz_metadata {
    /* Path to audio file (UTF-8, filtered from UTF-16-BE PPTH) */
    char audio_path[ANLZ_PATH_MAX];

    /* Beat grid (from PQTZ) */
    anlz_beat_t *beats;      /* heap, beat_count entries; NULL if absent   */
    uint16_t     beat_count; /* number of beat entries                     */
    uint16_t     bpm;        /* BPM rounded from first entry's bpm_x100    */

    /* Hot cues / loops (from PCOB/PCPT) */
    anlz_cue_t cues[ANLZ_MAX_CUES];
    uint8_t    cue_count;

    /* Memory cue (earliest entry of the type-0 PCOB, real PCPT layout).
     * memory_cue_ms is 0 when has_memory_cue is false. */
    uint32_t memory_cue_ms;
    bool     has_memory_cue;
    /* v309: the memory cues by time, earliest ANLZ_MAX_MEMORY_CUES only;
     * memory_cues[0] is the memory cue above when there is one. */
    anlz_memory_cue_t memory_cues[ANLZ_MAX_MEMORY_CUES];
    uint8_t  memory_cue_count;
    /* v303: a PCOB list (memory or hot cues) was read, even an empty one:
     * the analysis speaks for the track's cues. */
    bool     has_cue_lists;

    /* VBR seek table (from PVBR) — 400 file-byte offsets */
    uint32_t vbr[ANLZ_VBR_TABLE_LEN];
    bool     has_vbr;

    /* Low-resolution waveform (from PWAV) — 400 bytes */
    uint8_t waveform_low[ANLZ_WAVEFORM_LOW_LEN];
    bool    has_waveform_low;

    /* High-resolution waveform (from PWV3 in .EXT) — heap */
    uint8_t  *waveform_high;     /* heap; NULL until anlz_parse_ext() called */
    uint32_t  waveform_high_len; /* number of valid bytes in waveform_high   */

    /* v313: colour preview (PWV4 in .EXT, optional) — heap, raw entries of
     * ANLZ_COLOR_PREVIEW_ENTRY bytes. Measured on rekordbox exports: byte 0
     * follows the PWV3 height (r 0.89..0.97, peaks ~65..80), byte 1 a
     * luminance (~255), bytes 3/4/5 the bass / mid / treble intensities
     * (vynull tools/wavecompare). NULL / 0 when absent. */
    uint8_t  *color_preview;
    uint32_t  color_preview_len; /* bytes, a multiple of the entry size */
} anlz_metadata_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

/**
 * Parse ANLZ0000.DAT into *out.
 *
 * Reads PPTH, PVBR, PQTZ, PWAV, PCOB tags.
 * beats is heap-allocated; call anlz_free() when done.
 *
 * @param dat_path  Absolute path to ANLZ0000.DAT on the mounted USB drive.
 * @param out       Caller-allocated struct; zeroed on entry by this function.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND on failure.
 */
esp_err_t anlz_parse_dat(const char *dat_path, anlz_metadata_t *out);

/**
 * Parse ANLZ0000.EXT and populate the high-res waveform field.
 *
 * Must be called after anlz_parse_dat().  Reads the PWV3 tag.
 * waveform_high is heap-allocated; anlz_free() will release it.
 *
 * @param ext_path  Absolute path to ANLZ0000.EXT on the mounted USB drive.
 * @param meta      Already-parsed metadata struct from anlz_parse_dat().
 * @return ESP_OK on success.
 */
esp_err_t anlz_parse_ext(const char *ext_path, anlz_metadata_t *meta);

/** Deep-copy metadata, including heap-owned beats and high-resolution
 * waveform data. `out` must not already own allocations. */
esp_err_t anlz_clone(const anlz_metadata_t *src, anlz_metadata_t *out);

/**
 * Free all heap-allocated fields inside meta (beats, waveform_high).
 * Does NOT free the struct itself (caller-allocated).
 * Safe to call multiple times (idempotent).
 */
void anlz_free(anlz_metadata_t *meta);

/* v312: a 400-column overview preview from the PWV3 detail: the loudest
 * entry (height in bits 4:0) of each column, colour bits kept. It is the
 * rule dj_link_anlz_preview uses to write a DJ Link peer's PWAV, so a track
 * shows the same overview from the USB and from a peer. rekordbox's own
 * PWAV is much lower (an average: heights often 2..16 of 31, some tracks
 * nearly flat). False (out zeroed) without detail. */
bool anlz_preview_from_high(const uint8_t *high, size_t len,
                            uint8_t out[ANLZ_WAVEFORM_LOW_LEN]);

/* v313: one column of a PWV4 colour preview drawn `cols` columns wide.
 * height: the loudest entry's byte 0 over the column's entries (scale it
 * by anlz_color_preview_peak). r/g/b: the mean bass / mid / treble of those
 * entries, scaled so the strongest is 255; all 0 when the bands are silent
 * (the caller picks a colour). False when there is no preview. */
typedef struct {
    uint8_t height;
    uint8_t r, g, b;
} anlz_color_column_t;

bool anlz_color_preview_column(const uint8_t *preview, uint32_t len, uint32_t col,
                               uint32_t cols, anlz_color_column_t *out);
/* The loudest byte 0 of the whole preview (0 = none). */
uint8_t anlz_color_preview_peak(const uint8_t *preview, uint32_t len);

#ifdef __cplusplus
}
#endif
