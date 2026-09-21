/**
 * @file wifi_console.c
 * @brief Wi-Fi STA + TCP log console.
 *
 * Bring-up remote debug: the ESP-IDF log output is duplicated to a TCP
 * client connected on port 2333 (e.g. `telnet <ip> 2333`). The local
 * USB-Serial-JTAG console stays active as well.
 *
 * Bring-up only: credentials are hard-coded; move to NVS/provisioning later.
 */

#include "wifi_console.h"
#include "usb_tu_app.h"
#include "eth_bringup.h"

#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ota_update.h"
#include <stdlib.h>

#define WIFI_SSID        "kayrozen"
#define WIFI_PASSWORD    "4182624454"
#define CONSOLE_PORT     2333

static const char* TAG = "wifi_console";

static esp_netif_ip_info_t s_ip_info;
static bool s_got_ip = false;
static int s_client_sock = -1;
static SemaphoreHandle_t s_sock_mutex = NULL;

/* ------------------------------------------------------------------ */
/* Log duplication hook                                               */
/* ------------------------------------------------------------------ */

static int console_vprintf(const char* fmt, va_list args)
{
    /* Format once for the TCP client (va_copy: args is reused below). */
    va_list copy;
    va_copy(copy, args);
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    if (len > 0 && s_client_sock >= 0 && s_sock_mutex &&
        xSemaphoreTake(s_sock_mutex, 0) == pdTRUE) {
        int remaining = len;
        const char* p = buf;
        while (remaining > 0) {
            int sent = send(s_client_sock, p, remaining, 0);
            if (sent <= 0) {
                /* Client gone: close asynchronously, server task reaps it. */
                shutdown(s_client_sock, 0);
                break;
            }
            p += sent;
            remaining -= sent;
        }
        xSemaphoreGive(s_sock_mutex);
    }

    /* Chain to the previous/default output. */
    return vprintf(fmt, args);
}

/* ------------------------------------------------------------------ */
/* TCP server task                                                    */
/* ------------------------------------------------------------------ */

static void console_server_task(void* arg)
{
    /* v72: wait for an IP on ANY interface (Ethernet preferred path, Wi-Fi
     * fallback) instead of blocking the boot. */
    for (int i = 0; i < 1200 && !s_got_ip && !eth_bringup_got_ip(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_got_ip && !eth_bringup_got_ip()) {
        ESP_LOGE(TAG, "No IP after 120 s on any interface - console disabled");
        vTaskDelete(NULL);
        return;
    }
    if (eth_bringup_got_ip()) {
        s_ip_info = eth_bringup_ip_info();
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(CONSOLE_PORT),
    };
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen failed on port %d", CONSOLE_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Log console listening on %d.%d.%d.%d:%d",
             (s_ip_info.ip.addr >> 0) & 0xFF, (s_ip_info.ip.addr >> 8) & 0xFF,
             (s_ip_info.ip.addr >> 16) & 0xFF, (s_ip_info.ip.addr >> 24) & 0xFF,
             CONSOLE_PORT);

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int sock = accept(listen_sock, (struct sockaddr*)&client, &clen);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (xSemaphoreTake(s_sock_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_client_sock >= 0) {
                close(s_client_sock); /* only one client at a time */
            }
            s_client_sock = sock;
            xSemaphoreGive(s_sock_mutex);
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                     (unsigned)(ntohl(client.sin_addr.s_addr) >> 24) & 0xFF,
                     (unsigned)(ntohl(client.sin_addr.s_addr) >> 16) & 0xFF,
                     (unsigned)(ntohl(client.sin_addr.s_addr) >> 8) & 0xFF,
                     (unsigned)(ntohl(client.sin_addr.s_addr) >> 0) & 0xFF);
            ESP_LOGI(TAG, "Console client connected from %s", ip);
            ota_update_set_host(ip);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Wi-Fi events                                                       */
/* ------------------------------------------------------------------ */

static void wifi_event_cb(void* arg, esp_event_base_t base,
                          int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (usb_audio_wifi_suspended()) {
            /* Audio owns the radio right now (Wi-Fi suspended to protect the
             * USB iso stream from SDIO bus bursts). Do not reconnect until
             * audio_wifi_resume() hands it back. */
            ESP_LOGW(TAG, "Wi-Fi disconnected: audio owns the radio, not reconnecting");
            return;
        }
        wifi_event_sta_disconnected_t* evt = (wifi_event_sta_disconnected_t*)data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d", evt->reason);
        /* v78: do NOT call esp_wifi_connect() from the event handler - a
         * hanging C6 RPC here stalled the whole event loop (only ONE
         * disconnect was ever delivered). The watchdog task owns retrying. */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* evt = (ip_event_got_ip_t*)data;
        s_ip_info = evt->ip_info;
        s_got_ip = true;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

/* v77: diagnostic scan - when the association fails with 203, log what the
 * C6 radio actually sees (SSID, channel, RSSI, authmode) so we can tell a
 * band/channel/PMF problem from a weak-signal one. Runs from the watchdog
 * task, never from the boot path. */
static void scan_and_log(void)
{
    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&sc, true); /* blocking */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGW(TAG, "scan: NO APs visible at all");
        return;
    }
    if (n > 10) {
        n = 10;
    }
    wifi_ap_record_t recs[10];
    esp_wifi_scan_get_ap_records(&n, recs);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "scan: %02d %-20s ch=%2d rssi=%d auth=%d",
                 i, (const char*)recs[i].ssid, recs[i].primary,
                 recs[i].rssi, recs[i].authmode);
    }
}

/* v78: scan runs in a throwaway task - a hanging C6 scan RPC must not
 * kill the watchdog (the watchdog also delays its first connect while
 * the scan runs). */
static void scan_task(void* arg)
{
    scan_and_log();
    vTaskDelete(NULL);
}

/* v65: reconnect watchdog - independent of event delivery, retries every
 * 10 s as long as the station is down. */
static void wifi_reconnect_task(void* arg)
{
    bool first = true;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(first ? 3000 : 10000));
        if (usb_audio_wifi_suspended()) {
            continue;
        }
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_STA) {
            continue;
        }
        bool connected = s_got_ip;
        if (!connected) {
            if (first) {
                first = false;
                xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL);
                vTaskDelay(pdMS_TO_TICKS(6000)); /* let the scan finish */
            }
            esp_err_t err = esp_wifi_connect();
            ESP_LOGI(TAG, "watchdog: esp_wifi_connect -> %s",
                     esp_err_to_name(err));
        } else {
            first = false;
        }
    }
}

bool wifi_console_start(void)
{
    /* v59: scan results (AP list + RSSI) are ESP_LOGI - raise this tag so
     * the on-screen log shows what the C6 radio actually sees. */
    esp_log_level_set(TAG, ESP_LOG_INFO);
    s_sock_mutex = xSemaphoreCreateMutex();

    if (esp_netif_init() != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed");
        return false;
    }
    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed");
        return false;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_cb, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* v83: ESPHome (which connects fine) uses threshold OPEN - the
             * weakest authmode accepted; WPA2_PSK filter rejected the AP
             * and association never even started (203/2). The real auth
             * mode is negotiated from the AP beacon with our password. */
            .threshold.authmode = WIFI_AUTH_OPEN,
            .threshold.rssi = -127,
            .scan_method = WIFI_FAST_SCAN,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* v63: PS_NONE (v39) removed - it was added under the invalidated
     * "Wi-Fi steals the bus" theory and can break association with the
     * co-proc version mismatch. Default MIN_MODEM power-save is back. */

    /* v80: the initial esp_wifi_connect() used to be called here - but when
     * the association fails the C6 RPC never returns and the whole boot
     * stalls before the console/watchdog tasks are even created. The
     * watchdog task owns connecting now. */

    /* v72: tee logs to the TCP console from now on (the server task below
     * accepts connections as soon as ANY interface has an IP). */
    esp_log_set_vprintf(console_vprintf);

    if (xTaskCreate(console_server_task, "wifi_console", 4096, NULL, 4, NULL)
        != pdPASS) {
        return false;
    }
    if (xTaskCreate(wifi_reconnect_task, "wifi_reconn", 3072, NULL, 4, NULL)
        != pdPASS) {
        return false;
    }

    ESP_LOGI(TAG, "Wi-Fi connecting (async)...");
    return true;
}
