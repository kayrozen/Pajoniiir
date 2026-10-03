#pragma once

/*
 * Bounded microSD (FAT) I/O arbiter.
 *
 * Serialises administrative /sd FAT operations shared by the master recorder,
 * the track metadata cache, controller-profile install, the service log and
 * free-space queries, so none of them corrupt the FAT or starve another. The
 * recorder writer holds the gate only around bounded 32-64 KiB writes and
 * releases it between batches. No audio producer ever acquires this gate.
 */

#include <stdbool.h>
#include <stdint.h>

#if defined(SD_IO_GATE_STANDALONE_TEST)
#ifndef ESP_ERR_T_DEFINED
typedef int esp_err_t;
#define ESP_ERR_T_DEFINED
#endif
#define ESP_OK 0
#define ESP_FAIL -1
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Classes of microSD (FAT) work that share the bounded arbiter. */
typedef enum {
    SD_IO_CLASS_RECORDER = 0,     /* bounded recorder WAV segment writes */
    SD_IO_CLASS_META_CACHE,       /* track metadata cache read/write */
    SD_IO_CLASS_PROFILE_INSTALL,  /* controller-profile install/swap */
    SD_IO_CLASS_SERVICE_LOG,      /* service-log append/rotate */
    SD_IO_CLASS_FREE_SPACE,       /* free-space / status query */
    SD_IO_CLASS_PROFILE_UPLOAD,   /* large web controller-profile upload */
    SD_IO_CLASS_LOG_DOWNLOAD,     /* full diagnostic-log stream */
} sd_io_class_t;

/* Create the arbiter mutex (idempotent). */
esp_err_t sd_io_gate_init(void);

/* Serialise a bounded microSD/FAT administrative operation. begin() blocks;
 * try_begin() returns false on timeout so a caller can report BUSY instead of
 * stalling. Hold only around bounded work and release between batches. */
void sd_io_gate_begin(void);
bool sd_io_gate_try_begin(uint32_t timeout_ms);
void sd_io_gate_end(void);

/* Recorder-active awareness used by the admission policy below. */
void sd_io_gate_set_recorder_active(bool active);
bool sd_io_gate_recorder_active(void);

/* Pure admission policy: may an operation of `op_class` proceed now given the
 * recorder state? Heavy optional admin ops (large profile upload, full log
 * download) are refused while recording so they cannot back-pressure the
 * writer; bounded fast ops always proceed. */
bool sd_io_gate_admit(sd_io_class_t op_class, bool recorder_active);

/* v323: the SD/USB probes of the download-crackle HIL (v319-v323: these
 * diag marks, the mix "PROBE sdio" classes, the isochronous cadence probe,
 * the download write timings) only run in a diagnostics build, on the same
 * switch as ui_diagnostics.h: idf.py -DPAJONIIIR_DIAGNOSTICS=1 build. */
#ifndef UI_DIAGNOSTICS_ENABLED
#define UI_DIAGNOSTICS_ENABLED 0
#endif
#define SD_IO_DIAG_ENABLED (UI_DIAGNOSTICS_ENABLED != 0)

/* v319 DIAGNOSTIC (HIL of the download crackles): which SD transfer is in
 * flight when the audio mix stalls. A writer (dj_link download) and a reader
 * (deck loader page) mark their transfer with diag_begin / diag_end (task
 * context, not the audio path); the mix asks diag_overlaps only for a
 * stalled group, so a normal block pays nothing. Times are the low 32 bits
 * of esp_timer_get_time() (us; wrap-safe differences). */
typedef enum {
    SD_IO_DIAG_WRITE = 0,
    SD_IO_DIAG_READ,
    SD_IO_DIAG_KINDS,
} sd_io_diag_kind_t;

extern uint32_t g_sd_io_diag_active[SD_IO_DIAG_KINDS];   /* transfers in flight */
extern uint32_t g_sd_io_diag_end_us[SD_IO_DIAG_KINDS];   /* last transfer end */

void sd_io_gate_diag_begin(sd_io_diag_kind_t kind);
void sd_io_gate_diag_end(sd_io_diag_kind_t kind);

/* A transfer of `kind` is in flight now, or ended at or after since_us. */
static inline bool sd_io_gate_diag_overlaps(sd_io_diag_kind_t kind, uint32_t since_us)
{
    if (!SD_IO_DIAG_ENABLED) {
        return false;
    }
    if (__atomic_load_n(&g_sd_io_diag_active[kind], __ATOMIC_RELAXED) != 0u) {
        return true;
    }
    const uint32_t end = __atomic_load_n(&g_sd_io_diag_end_us[kind], __ATOMIC_RELAXED);
    return (int32_t)(end - since_us) >= 0;
}

#ifdef __cplusplus
}
#endif
