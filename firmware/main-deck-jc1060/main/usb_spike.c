/* J0 spike: ONE usb_host_install owning BOTH ports (BIT0|BIT1), the
 * configuration proven upstream (esp-usb PR #402 / issue #396). An async
 * client callback logs every NEW_DEV with VID:PID so we can verify:
 *   - DDJ-FLX4/400 (2b73:xxxx) enumerates on the HS port (BIT0), and
 *   - the MSC stick enumerates on the FS port (BIT1, FSLS PHY0 after the
 *     v148 phy_select swap to GPIO24/25),
 *   - simultaneously.
 * Diagnostic only - replaces usb_storage/TinyUSB for this build (v150). */
#include "usb_spike.h"

#include "esp_log.h"
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define SPIKE_TASK_STACK 6144
static const char *TAG = "usb_spike";

static usb_host_client_handle_t spike_client;

static void spike_client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t addr = msg->new_dev.address;
        usb_device_handle_t dev = NULL;
        usb_host_client_handle_t client = spike_client;
        if (usb_host_device_open(client, addr, &dev) == ESP_OK) {
            const usb_device_desc_t *d = NULL;
            if (usb_host_get_device_descriptor(dev, &d) == ESP_OK && d) {
                ESP_LOGW(TAG, "DEV_NEW addr=%u VID=0x%04x PID=0x%04x class=0x%02x",
                         addr, d->idVendor, d->idProduct, d->bDeviceClass);
            } else {
                ESP_LOGW(TAG, "DEV_NEW addr=%u (desc read failed)", addr);
            }
            usb_host_device_close(client, dev);
        } else {
            ESP_LOGW(TAG, "DEV_NEW addr=%u (open failed)", addr);
        }
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "DEV_GONE");
    }
}

static void spike_task(void *arg)
{
    (void)arg;
    /* v154: NO phy_select - hypothesis reversed. If the JC1060 "FS"
     * connector is actually wired to GPIO26/27 (USB1P1 = default OTG-FS
     * PHY1), the default mapping was right all along and the v148 swap
     * moved OTG-FS to pins that go nowhere. */
    const usb_host_config_t cfg = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        /* v153: let the library power the root ports itself at install
         * time (standard mode). The manual power-on after the fact never
         * produced a single attach event. */
        .root_port_unpowered = false,
        .peripheral_map = BIT(0) | BIT(1), /* BOTH ports, one install */
    };
    esp_err_t err = usb_host_install(&cfg);
    ESP_LOGW(TAG, "usb_host_install(BIT0|BIT1) -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }

    usb_host_client_config_t ccfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = spike_client_cb, .callback_arg = NULL },
    };
    usb_host_client_handle_t client = NULL;
    err = usb_host_client_register(&ccfg, &client);
    spike_client = client;
    ESP_LOGW(TAG, "client_register -> %s", esp_err_to_name(err));

    /* v151: root_port_unpowered=true means WE must power the root port(s).
     * espressif/usb 1.5.0: single-arg API (powers the mapped ports). */
    esp_err_t rc = usb_host_lib_set_root_port_power(true);
    ESP_LOGW(TAG, "root_port_power(true) -> %s", esp_err_to_name(rc));

    /* v153: periodic device count - proves whether the hub enumerates. */
    while (1) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
        static int tick;
        if ((++tick % 40) == 0) {
            usb_host_lib_info_t info = { 0 };
            if (usb_host_lib_info(&info) == ESP_OK) {
                ESP_LOGW(TAG, "lib_info: devices=%d clients=%d",
                         info.num_devices, info.num_clients);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void usb_spike_start(void)
{
    if (xTaskCreate(spike_task, "usb_spike", SPIKE_TASK_STACK, NULL, 4, NULL)
        != pdPASS) {
        ESP_LOGE(TAG, "spike task create failed");
    }
}
