/**
 * @file usb_host_bringup.c
 * @brief USB host bring-up: enumerate every device behind the hub and report
 *        its class (MIDI / MSC / HID / hub).
 *
 * Standard usb_host_lib layout:
 *  - daemon task: usb_host_install() + usb_host_lib_handle_events() loop
 *  - enumerator task: registers a client whose callback queues NEW_DEV
 *    events; each device is opened, classified, then closed.
 *
 * Bring-up only: no class driver is attached yet. The DDJ-400 (MIDI) and the
 * mass-storage device will be handed to their drivers in a later phase.
 */

#include "usb_host_bringup.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_helpers.h"
#include "midi_host.h"
#include "usb_audio_out.h"

static const char* TAG = "usb_bringup";

#define USB_DAEMON_TASK_STACK   (4096)
#define USB_ENUM_TASK_STACK     (6144)
#define USB_DAEMON_TASK_PRIO    (2)
#define USB_ENUM_TASK_PRIO      (3)

static QueueHandle_t s_new_dev_queue = NULL;
static QueueHandle_t s_gone_dev_queue = NULL;
static usb_host_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_installed_sem = NULL;

/* ------------------------------------------------------------------ */
/* Device classification                                              */
/* ------------------------------------------------------------------ */

/* Returns true when the device should stay open (claimed by a sub-driver). */
static bool classify_device(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    const usb_device_desc_t* dev_desc = NULL;
    const usb_config_desc_t* cfg_desc = NULL;

    if (usb_host_get_device_descriptor(dev_hdl, &dev_desc) != ESP_OK ||
        usb_host_get_active_config_descriptor(dev_hdl, &cfg_desc) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read descriptors");
        return false;
    }

    ESP_LOGI(TAG, "Device addr=%u: VID=0x%04X PID=0x%04X, class=0x%02X",
             dev_addr, dev_desc->idVendor, dev_desc->idProduct,
             dev_desc->bDeviceClass);

    bool is_midi = false;
    bool is_audio = false;
    bool is_msc = false;
    bool is_hub = false;
    bool is_hid = false;

    /* Dump all interfaces incl. alternate settings (audio alt settings
     * carry the isochronous endpoints we need for the deck audio path). */
    {
        int off = 0;
        const uint8_t* p = (const uint8_t*)cfg_desc;
        int total = cfg_desc->wTotalLength;
        while (off + 2 <= total) {
            uint8_t len = p[off];
            uint8_t type = p[off + 1];
            if (len < 2 || off + len > total) {
                break;
            }
            if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                const usb_intf_desc_t* ifc =
                    (const usb_intf_desc_t*)(p + off);
                ESP_LOGI(TAG, "  DUMP ifc %d alt %d: class=0x%02X sub=0x%02X, %d EP",
                         ifc->bInterfaceNumber, ifc->bAlternateSetting,
                         ifc->bInterfaceClass, ifc->bInterfaceSubClass,
                         ifc->bNumEndpoints);
            } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
                const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(p + off);
                ESP_LOGI(TAG, "  DUMP   ep 0x%02X attr=0x%02X mps=%d interval=%d",
                         ep->bEndpointAddress, ep->bmAttributes,
                         ep->wMaxPacketSize, ep->bInterval);
            } else if (type == 0x24 || type == 0x25) {
                /* Class-specific audio descriptors: CS_INTERFACE / CS_ENDPOINT */
                ESP_LOGI(TAG, "  DUMP cs type=0x%02X sub=0x%02X len=%d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                         type, p[off + 2], len,
                         len > 4 ? p[off+3] : 0, len > 5 ? p[off+4] : 0,
                         len > 6 ? p[off+5] : 0, len > 7 ? p[off+6] : 0,
                         len > 8 ? p[off+7] : 0, len > 9 ? p[off+8] : 0,
                         len > 10 ? p[off+9] : 0, len > 11 ? p[off+10] : 0,
                         len > 12 ? p[off+11] : 0, len > 13 ? p[off+12] : 0,
                         len > 14 ? p[off+13] : 0, len > 15 ? p[off+14] : 0);
            }
            off += len;
        }
    }

    for (int i = 0; i < cfg_desc->bNumInterfaces; i++) {
        int offset = 0;
        const usb_intf_desc_t* ifc =
            usb_parse_interface_descriptor(cfg_desc, i, 0, &offset);
        if (ifc == NULL) {
            continue;
        }
        ESP_LOGI(TAG, "  ifc %d: class=0x%02X sub=0x%02X proto=0x%02X, %d EP",
                 i, ifc->bInterfaceClass, ifc->bInterfaceSubClass,
                 ifc->bInterfaceProtocol, ifc->bNumEndpoints);

        switch (ifc->bInterfaceClass) {
            case 0x01: /* Audio */
                is_audio = true;
                if (ifc->bInterfaceSubClass == 0x03) {
                    is_midi = true;   /* MIDI streaming */
                }
                break;
            case 0x03: is_hid = true; break;   /* HID */
            case 0x08: is_msc = true; break;   /* MSC/BOT */
            case 0x09: is_hub = true; break;   /* Hub */
            default: break;
        }
    }

    if (is_hub)        ESP_LOGI(TAG, ">>> HUB detected");
    if (is_midi)       ESP_LOGI(TAG, ">>> MIDI device detected (DDJ-400?)");
    else if (is_audio) ESP_LOGI(TAG, ">>> Audio device detected");
    if (is_msc)        ESP_LOGI(TAG, ">>> Mass-storage device detected");
    if (is_hid)        ESP_LOGI(TAG, ">>> HID device detected");
    if (!is_midi && !is_audio && !is_msc && !is_hub && !is_hid) {
        ESP_LOGI(TAG, ">>> Unclassified device");
    }

    if (is_midi) {
        midi_host_attach(dev_hdl, cfg_desc);
        usb_audio_out_attach(s_client, dev_hdl, cfg_desc);
        return true; /* keep open: MIDI + audio both claimed */
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Client callback (runs from usb_host_client_handle_events context)  */
/* ------------------------------------------------------------------ */

static void client_event_cb(const usb_host_client_event_msg_t* msg, void* arg)
{
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            ESP_LOGI(TAG, "New device at addr %u", msg->new_dev.address);
            if (s_new_dev_queue) {
                xQueueSend(s_new_dev_queue, &msg->new_dev.address, portMAX_DELAY);
            }
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGW(TAG, "Device gone");
            if (s_gone_dev_queue) {
                xQueueSend(s_gone_dev_queue, &msg->dev_gone.dev_hdl, portMAX_DELAY);
            }
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Daemon task                                                        */
/* ------------------------------------------------------------------ */

static void daemon_task(void* arg)
{
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        /* Custom FIFO split (P4 DWC DFIFO = 512 lines total, 4 bytes/line).
        ...[truncated]
         * DDJ-400 needs bulk IN/OUT MPS 512 (MIDI) AND iso OUT MPS 576
         * (audio) concurrently - no stock bias preset satisfies both.
         * Wide margins like esp-uac2-host: total 500 <= 512 lines.
         *   rx  160 lines -> in_mps  = (160-2)*4  = 632 >= 512
         *   nptx 160 lines -> nptx_mps = 640 >= 512
         *   ptx  180 lines -> ptx_mps = 720 >= 576
         */
        .fifo_settings_custom = {
            .rx_fifo_lines = 160,
            .nptx_fifo_lines = 160,
            .ptx_fifo_lines = 180,
        },
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB host library installed, waiting for devices...");
    xSemaphoreGive(s_installed_sem);

    while (1) {
        uint32_t event_flags = 0;
        if (usb_host_lib_handle_events(pdMS_TO_TICKS(1000), &event_flags) == ESP_OK) {
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGW(TAG, "No clients registered");
            }
        }
    }
    vTaskDelete(NULL); /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Enumerator task                                                    */
/* ------------------------------------------------------------------ */

static void enum_task(void* arg)
{
    /* Wait until the daemon has installed the host library. */
    xSemaphoreTake(s_installed_sem, portMAX_DELAY);

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    if (usb_host_client_register(&client_config, &s_client) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed");
        vTaskDelete(NULL);
        return;
    }
    midi_host_init(s_client);

    uint8_t dev_addr;
    usb_device_handle_t midi_dev = NULL;
    while (1) {
        /* Pump client events (runs client_event_cb) then drain the queues. */
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));
        while (xQueueReceive(s_new_dev_queue, &dev_addr, 0) == pdTRUE) {
            usb_device_handle_t dev_hdl = NULL;
            if (usb_host_device_open(s_client, dev_addr, &dev_hdl) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open device %u", dev_addr);
                continue;
            }
            if (classify_device(dev_addr, dev_hdl)) {
                /* MIDI claimed: keep the device open while attached. */
                midi_dev = dev_hdl;
            } else {
                usb_host_device_close(s_client, dev_hdl);
            }
        }
        usb_device_handle_t gone;
        while (xQueueReceive(s_gone_dev_queue, &gone, 0) == pdTRUE) {
            midi_host_detach(gone);
            usb_audio_out_detach(gone);
            usb_host_device_close(s_client, gone);
            if (midi_dev == gone) {
                midi_dev = NULL;
            }
        }
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

bool usb_host_bringup_start(void)
{
    s_installed_sem = xSemaphoreCreateBinary();
    s_new_dev_queue = xQueueCreate(8, sizeof(uint8_t));
    s_gone_dev_queue = xQueueCreate(8, sizeof(usb_device_handle_t));
    if (s_new_dev_queue == NULL) {
        return false;
    }
    if (xTaskCreatePinnedToCore(daemon_task, "usb_daemon", USB_DAEMON_TASK_STACK,
                                NULL, USB_DAEMON_TASK_PRIO, NULL, 0) != pdPASS) {
        return false;
    }
    if (xTaskCreatePinnedToCore(enum_task, "usb_enum", USB_ENUM_TASK_STACK,
                                NULL, USB_ENUM_TASK_PRIO, NULL, 1) != pdPASS) {
        return false;
    }
    ESP_LOGI(TAG, "USB host bring-up tasks started");
    return true;
}
