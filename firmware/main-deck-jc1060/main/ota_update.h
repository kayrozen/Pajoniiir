/**
 * @file ota_update.h
 * @brief Pull OTA over Wi-Fi/LAN for the JC1060 bring-up firmware.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the background OTA check task.
 *
 * The OTA image is fetched from http://<host>:8080/esp_draw_bit.bin where
 * <host> is the IP of the last Wi-Fi console (telnet) client - i.e. run
 * `python3 -m http.server 8080` in the build directory on the PC and
 * telnet to the board to trigger a check.
 *
 * Non-fatal: logs and retries periodically.
 */
void ota_update_start(void);

/**
 * @brief Register the IP (dotted string) of the OTA host (console client).
 */
void ota_update_set_host(const char* ip_str);

#ifdef __cplusplus
}
#endif
