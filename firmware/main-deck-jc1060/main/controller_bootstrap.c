/* jc1060 controller bootstrap - faithful port of the upstream
 * p4_local_controller flow (wait for the shared USB Host manager, then
 * bring up the HostLib controller path). The profile/runtime layers are
 * not ported yet; MIDI and connection events are logged. Board delta:
 * the DDJ sits on the HS DWC, which the manager exposes as root index 0
 * (upstream uses LOCAL_USB1_ROOT_INDEX for the same physical role). */
#include "controller_usb_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_host_manager.h"

static const char *TAG = "ctrl_boot";

/* v168 layout A: controller on the HS root (bottom connector), hub+stick
 * on the FS root (top). */
#define JC1060_CONTROLLER_ROOT_INDEX 0u

static void local_midi_callback(const usb_midi_message_t *message,
                                void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "MIDI status=0x%02x d1=0x%02x d2=0x%02x cable=%u",
             message->status, message->data1, message->data2,
             (unsigned)message->cable);
}

static void local_connection_callback(
    bool connected, const controller_usb_identity_t *identity, void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "controller %s VID:0x%04x PID:0x%04x speed=%u in_ep=0x%02x mps=%u",
             connected ? "CONNECTED" : "disconnected",
             identity ? identity->vid : 0, identity ? identity->pid : 0,
             identity ? (unsigned)identity->speed : 0u,
             identity ? (unsigned)identity->midi.in_ep_addr : 0u,
             identity ? (unsigned)identity->midi.in_ep_mps : 0u);
}

static void controller_bootstrap_task(void *arg)
{
    (void)arg;
    uint32_t manager_wait_ms = 0u;
    while (!usb_host_manager_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        manager_wait_ms += 100u;
        if (manager_wait_ms >= 10000u) {
            ESP_LOGW(TAG, "waiting for shared USB Host manager");
            manager_wait_ms = 0u;
        }
    }

    const controller_usb_host_config_t usb_config = {
        .midi_cb = local_midi_callback,
        .connection_cb = local_connection_callback,
        .callback_ctx = NULL,
        .task_stack_size = 8192u,
        .task_priority = 5u,
        .task_core_id = tskNO_AFFINITY,
        .midi_out_queue_depth = 256u,
        .max_event_messages = 8,
    };
    esp_err_t rc = ESP_ERR_INVALID_STATE;
    for (;;) {
        rc = controller_usb_host_init(&usb_config);
        if (rc == ESP_OK || rc == ESP_ERR_INVALID_STATE) {
            rc = usb_host_manager_set_root_power_by_index(
                JC1060_CONTROLLER_ROOT_INDEX, true);
        }
        if (rc == ESP_OK || rc == ESP_ERR_INVALID_STATE) {
            break;
        }
        ESP_LOGW(TAG, "controller bootstrap retry rc=%s", esp_err_to_name(rc));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "jc1060 controller path ready on root %u",
             (unsigned)JC1060_CONTROLLER_ROOT_INDEX);
    vTaskDelete(NULL);
}

void controller_bootstrap_start(void)
{
    if (xTaskCreate(controller_bootstrap_task, "jc_ctrl_boot", 6144u,
                    NULL, 3u, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create controller bootstrap task");
    }
}
