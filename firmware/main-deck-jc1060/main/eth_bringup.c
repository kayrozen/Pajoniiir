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
#include "driver/gpio.h"

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

    /* v90: make this component's INFO logs visible (screen log tee). */
    esp_log_level_set(TAG, ESP_LOG_INFO);
    /* v123: OTA checks are INFO - make them visible on the UART capture. */
    esp_log_level_set("ota_update", ESP_LOG_INFO);
    /* v104-night: DEBUG on the eth stack to see autonego/link poll state. */
    esp_log_level_set("eth_phy_802_3", ESP_LOG_DEBUG);
    esp_log_level_set("emac_esp", ESP_LOG_DEBUG);
    esp_log_level_set("esp_eth.netif.netif_glue", ESP_LOG_DEBUG);

    /* v91: step markers in WARN level - always visible on the screen log,
     * they localize exactly where bring-up stalls (if it does). */
    ESP_LOGW(TAG, "step 1: event loop + netif init");

    /* v113: no manual GPIO51 handling, no power settle - the IDF driver
     * owns the PHY reset (reset_gpio_num) exactly like the working vendor
     * example. Every deviation from the example is a suspect. */
    ESP_LOGW(TAG, "step 2: (defaults - no manual power settle)");

    /* ETH_EVENT needs a default event loop; nothing else created one yet. */
    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(loop_ret));
        return false;
    }

    /* v89: netif init used to rely on wifi_console_start running first. With
     * the Wi-Fi path parked, bring it up here (idempotent). */
    esp_err_t netif_ret = esp_netif_init();
    if (netif_ret != ESP_OK && netif_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(netif_ret));
        return false;
    }

    eth_esp32_emac_config_t mac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    /* v113: ALL defaults, exactly like the working vendor example
     * (P4 defaults are already MDC=31, MDIO=52, CLK_EXT_IN GPIO50). */
    eth_mac_config_t mac_time_config = ETH_MAC_DEFAULT_CONFIG();
    ESP_LOGW(TAG, "step 3: creating MAC/PHY objects");
    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&mac_config, &mac_time_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;              /* IP101 SMI address (EspControl) */
    /* v113: autonego/post-reset back to defaults (100 ms / 0). */
    phy_config.reset_gpio_num = 51;
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config); /* IDF6: generic covers IP101 (802.3) */

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_LOGW(TAG, "step 4: installing driver");
    esp_err_t ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* v112: MDIO scan DISABLED - with the scan enabled the eth stack stays
     * silent after start AND the LVGL display flickers (eth task suspected
     * of spinning on stuck-SMI reads). The vendor example (no pre-start
     * scan) gets Link Up + IP on this board, so the driver must own the
     * SMI bus from install onward. */
#if 0
    for (int addr = 0; addr < 32; addr++) {
        uint32_t bmsr = 0;
        if (mac->read_phy_reg(mac, addr, 0x01, &bmsr) == ESP_OK &&
            bmsr != 0x0000 && bmsr != 0xffff) {
            uint32_t id1 = 0, id2 = 0;
            mac->read_phy_reg(mac, addr, 0x02, &id1);
            mac->read_phy_reg(mac, addr, 0x03, &id2);
            ESP_LOGW(TAG, "MDIO scan: addr=%d BMSR=0x%04lx ID=0x%04lx%04lx",
                     addr, bmsr, id1, id2);
        }
    }
#endif

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
    ESP_LOGW(TAG, "step 5: netif attached");

    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGW(TAG, "step 6: eth started, waiting for link/DHCP");

    s_started = true;
    ESP_LOGI(TAG, "Ethernet bring-up started (RMII, IP101 @ addr 1)");
    return true;
}