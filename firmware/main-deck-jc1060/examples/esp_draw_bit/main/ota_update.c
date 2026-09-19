/**
 * @file ota_update.c
 * @brief Pull OTA over LAN for the JC1060 bring-up firmware.
 *
 * Flow:
 *  - The Wi-Fi console (telnet) client IP is used as the OTA host: run
 *    `python3 -m http.server 8080` in the build directory on the PC, telnet
 *    to the board, and the board fetches http://<pc>:8080/esp_draw_bit.bin.
 *  - Plain HTTP is used on the LAN (allow_http) - signed OTA over HTTPS is
 *    the production path (see main-deck-p4 OTA stack).
 *  - App rollback is enabled (sdkconfig): the new image must be marked valid
 *    (esp_ota_mark_app_valid_cancel_rollback after a stable uptime) or the
 *    bootloader falls back to the previous slot.
 */

#include "ota_update.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_TASK_STACK      (8192)
#define OTA_CHECK_PERIOD_S  (60)
#define OTA_HTTP_PORT       8080

static const char* TAG = "ota_update";

/* Set by the Wi-Fi console when a telnet client connects. */
void wifi_console_set_last_client_ip(uint32_t ip_host_order);

static char s_ota_host[16] = "192.168.100.20"; /* fallback */
static volatile bool s_host_known = false;

void ota_update_set_host(const char* ip_str)
{
    strncpy(s_ota_host, ip_str, sizeof(s_ota_host) - 1);
    s_ota_host[sizeof(s_ota_host) - 1] = '\0';
    s_host_known = true;
    ESP_LOGI(TAG, "OTA host set to %s", s_ota_host);
}

static void ota_task(void* arg)
{
    /* Mark this image valid once we survived 30 s (rollback safety). */
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_ota_mark_app_valid_cancel_rollback();

    while (1) {
        if (!s_host_known) {
            vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_S * 1000));
            continue;
        }

        char url[96];
        snprintf(url, sizeof(url), "http://%s:%d/esp_draw_bit.bin",
                 s_ota_host, OTA_HTTP_PORT);

        const esp_app_desc_t* cur = esp_app_get_description();
        ESP_LOGI(TAG, "OTA check: %s (running '%s' %s)", url, cur->project_name,
                 cur->version);

        esp_http_client_config_t http_config = {
            .url = url,
            .timeout_ms = 10000,
            .keep_alive_enable = false,
        };
        esp_https_ota_config_t ota_config = {
            .http_config = &http_config,
        };

        esp_https_ota_handle_t handle = NULL;
        esp_err_t ret = esp_https_ota_begin(&ota_config, &handle);
        if (ret != ESP_OK) {
            /* No server reachable / no image yet: silently retry later. */
            ESP_LOGD(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_S * 1000));
            continue;
        }

        /* Skip if the offered image is the same as the running one. */
        const esp_app_desc_t* running = esp_app_get_description();
        esp_app_desc_t new_desc = {0};
        bool apply = true;
        if (esp_https_ota_get_img_desc(handle, &new_desc) == ESP_OK) {
            if (strcmp(new_desc.version, running->version) == 0 &&
                strcmp(new_desc.project_name, running->project_name) == 0) {
                ESP_LOGI(TAG, "Same version '%s' already running, skipping OTA",
                         new_desc.version);
                apply = false;
            } else {
                ESP_LOGI(TAG, "New image '%s' %s (running '%s' %s)",
                         new_desc.project_name, new_desc.version,
                         running->project_name, running->version);
            }
        }
        if (!apply) {
            esp_https_ota_finish(handle);
            vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_S * 1000));
            continue;
        }

        while (1) {
            ret = esp_https_ota_perform(handle);
            if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (ret == ESP_OK) {
            ret = esp_https_ota_finish(handle);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "OTA image validated, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
            ESP_LOGE(TAG, "OTA finish: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
            esp_https_ota_finish(handle);
        }

        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_S * 1000));
    }
}

void ota_update_start(void)
{
    if (xTaskCreate(ota_task, "ota_update", OTA_TASK_STACK, NULL, 3, NULL)
        != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
    }
}
