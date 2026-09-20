/**
 * @file eth_bringup.c
 * @brief Ethernet bring-up (RMII EMAC + IP101 PHY) for JC1060P470C_I_W_Y.
 *
 * Detection/diagnostic only for now: logs link up/down so the path is proven
 * for DJ Link (ODE) integration later. No netif/DHCP attached yet.
 */

#include "eth_bringup.h"

#include "esp_log.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char* TAG = "eth_bringup";

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t* s_eth_netif = NULL;
static bool s_started = false;
static bool s_got_ip = false;
static esp_netif_ip_info_t s_ip_info;

/** v72: true once the Ethernet interface got an IP (DHCP). */
bool eth_bringup_got_ip(void)
{
    return s_got_ip;
}

esp_netif_ip_info_t eth_bringup_ip_info(void)
{
    return s_ip_info;
}

static void ip_event_cb(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t* evt = (ip_event_got_ip_t*)event_data;
        s_ip_info = evt->ip_info;
        s_got_ip = true;
        ESP_LOGI(TAG, "ETH Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

static void eth_event_cb(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "PHY link UP");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "PHY link DOWN");
            s_got_ip = false;
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet driver started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet driver stopped");
            break;
        default:
            break;
    }
}

bool eth_bringup_start(void)
{
    if (s_started) {
        return true;
    }

    /* ETH_EVENT needs a default event loop; nothing else created one yet. */
    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(loop_ret));
        return false;
    }

    eth_esp32_emac_config_t mac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    /* Defaults match the JC1060P470C board: MDC=31, MDIO=52,
     * RMII REF_CLK input on GPIO50 (50 MHz from the IP101 PHY). */
    eth_mac_config_t mac_time_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&mac_config, &mac_time_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;              /* IP101 SMI address on this board */
    phy_config.autonego_timeout_ms = 1000;
    phy_config.reset_gpio_num = -1; /* PHY reset handled by board circuitry */
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config); /* IP101 is 802.3 */

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event handler register failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler register failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* v72: attach a netif with DHCP so the TCP debug console is reachable
     * over Ethernet (esp_netif_init already done by wifi_console). */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return false;
    }
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
        return false;
    }

    s_started = true;
    ESP_LOGI(TAG, "Ethernet bring-up started (RMII, IP101 @ addr 1)");
    return true;
}