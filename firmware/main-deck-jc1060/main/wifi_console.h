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

/**
 * @brief Start ONLY the TCP log console (port 2333) - no Wi-Fi.
 *
 * v89: installs the log tee and spawns the server task, which accepts
 * connections as soon as ANY interface (Ethernet) has an IP. Use this when
 * the Wi-Fi path is parked (#if 0 in main.c).
 *
 * @return true if the console task was created
 */
bool console_tcp_start(void);

#ifdef __cplusplus
}
#endif
