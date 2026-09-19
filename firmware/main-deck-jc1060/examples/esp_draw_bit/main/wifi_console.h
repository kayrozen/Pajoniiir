/**
 * @file wifi_console.h
 * @brief Wi-Fi STA + TCP log console (port 2333) for remote bring-up debug.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect Wi-Fi STA and start the TCP log console.
 *
 * All ESP-IDF logs are duplicated to any connected TCP client on port 2333
 * (telnet-friendly). Returns after the console is started (non-blocking
 * server task); Wi-Fi association happens inside and is logged.
 *
 * @return true if Wi-Fi got an IP address
 */
bool wifi_console_start(void);

#ifdef __cplusplus
}
#endif
