/**
 * @file eth_bringup.h
 * @brief Ethernet (RMII + IP101) bring-up for JC1060P470C_I_W_Y - DJ Link later.
 */

#pragma once

#include <stdbool.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the EMAC + IP101 PHY, netif and DHCP.
 *
 * P4 RMII pins are dedicated pads; defaults match this board
 * (MDC=31, MDIO=52, REF_CLK input on GPIO50, PHY addr 1).
 * Non-fatal: logs link up/down events.
 *
 * @return true if the Ethernet driver started successfully
 */
bool eth_bringup_start(void);

/** v72: true once the Ethernet interface obtained an IP via DHCP. */
bool eth_bringup_got_ip(void);

/** v72: current Ethernet IP info (valid after eth_bringup_got_ip()). */
esp_netif_ip_info_t eth_bringup_ip_info(void);

#ifdef __cplusplus
}
#endif
