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
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d, retrying in 3 s...",
                 evt->reason);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* evt = (ip_event_got_ip_t*)data;
        s_ip_info = evt->ip_info;
        s_got_ip = true;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

bool wifi_console_start(void)
{
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
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* No power-save: PS bursts monopolize the SDIO bus and glitch USB audio. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* RPC health probe: scan and check the target AP is visible. */
    {
        wifi_scan_config_t scan = { .show_hidden = true };
        if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            wifi_ap_record_t* aps = malloc(sizeof(wifi_ap_record_t) * (n ? n : 1));
            if (aps) {
                esp_wifi_scan_get_ap_records(&n, aps);
                ESP_LOGI(TAG, "Scan: %u AP(s) visibles", n);
                bool found = false;
                for (int i = 0; i < n; i++) {
                    ESP_LOGI(TAG, "  %2d: %s ch=%d rssi=%d", i, aps[i].ssid,
                             aps[i].primary, aps[i].rssi);
                    if (strcmp((char*)aps[i].ssid, WIFI_SSID) == 0) {
                        found = true;
                    }
                }
                if (!found) {
                    ESP_LOGW(TAG, "SSID '%s' non visible au scan !", WIFI_SSID);
                }
                free(aps);
            }
        } else {
            ESP_LOGE(TAG, "esp_wifi_scan_start failed (RPC vers C6 ?)");
        }
    }

    /* Association: without this the STA starts but never associates. */
    esp_err_t conn_ret = esp_wifi_connect();
    if (conn_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(conn_ret));
    }

    /* Wait for IP (up to 60 s - first association can be slow). */
    for (int i = 0; i < 600 && !s_got_ip; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_got_ip) {
        ESP_LOGE(TAG, "No IP address obtained");
        return false;
    }

    /* Duplicate logs to the TCP console. */
    esp_log_set_vprintf(console_vprintf);

    if (xTaskCreate(console_server_task, "wifi_console", 4096, NULL, 4, NULL)
        != pdPASS) {
        return false;
    }

    ESP_LOGI(TAG, "Wi-Fi console ready");
    return true;
}
