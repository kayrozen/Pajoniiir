#pragma once
//
// dj_link_nfsd_service (v342) - the task that serves dj_link_nfsd on the
// Ethernet address: the portmapper (UDP 111) and one port for MOUNT + NFS
// (2049), each a BSD socket bound to our own IP, so nothing on the Wi-Fi AP can
// reach the export. One task, priority 1 like dj_link, core 1, stack and
// buffers in PSRAM. It exports `root` (the mounted USB key) read-only
// while `available()` says the key is mounted, and closes its cached file and
// directory when idle or when the key goes.
//
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *root;                 /* "/usb" */
    bool (*available)(void);          /* the key is mounted */
    void (*gate_begin)(void);         /* around every access to the medium, NULL = none */
    void (*gate_end)(void);
} dj_link_nfsd_service_cfg_t;

/* Start serving on ip (host order). ESP_ERR_INVALID_STATE if already running;
 * the sockets or the task failing is an error and nothing is left running. */
esp_err_t dj_link_nfsd_service_start(const dj_link_nfsd_service_cfg_t *cfg, uint32_t ip);
/* Stop and free the sockets (idempotent). */
void dj_link_nfsd_service_stop(void);

#ifdef __cplusplus
}
#endif
