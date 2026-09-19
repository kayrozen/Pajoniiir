/**
 * @file eth_bringup.h
 * @brief Ethernet (RMII + IP101) bring-up for JC1060P470C_I_W_Y - DJ Link later.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the EMAC + IP101 PHY and report link status.
 *
 * P4 RMII pins are dedicated pads; defaults match this board
 * (MDC=31, MDIO=52, REF_CLK input on GPIO50, PHY addr 1).
 * Non-fatal: logs link up/down events.
 *
 * @return true if the Ethernet driver started successfully
 */
bool eth_bringup_start(void);

#ifdef __cplusplus
}
#endif
