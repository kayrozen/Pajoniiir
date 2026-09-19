/**
 * @file midi_host.c
 * @brief Minimal USB MIDI host: claims the MIDI streaming interface of the
 *        DDJ-400 (class 0x01/0x03) and polls the bulk IN endpoint.
 *
 * USB-MIDI 1.0 event packets (4 bytes) are parsed and note on/off, CC and
 * pitch bend messages are logged. This is the reception path for the
 * DDJ-400 controls; the mapping to deck actions comes later.
 */

#include "midi_host.h"

#include <string.h>
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_helpers.h"

#define MIDI_TAG "midi_host"

#define MIDI_XFER_BUF_LEN 512

typedef struct {
    usb_device_handle_t dev_hdl;
    usb_transfer_t* in_xfer;
    uint8_t intf_num;
    bool claimed;
    bool running;
} midi_dev_t;

static midi_dev_t s_midi;
static usb_host_client_handle_t s_client = NULL;

/* ------------------------------------------------------------------ */
/* USB-MIDI event packet parsing                                      */
/* ------------------------------------------------------------------ */

static void midi_log_event(const uint8_t* pkt)
{
    uint8_t cin = pkt[0] & 0x0F;
    uint8_t status = pkt[1];
    if (status < 0x80) {
        return; /* padded bytes, not an event */
    }
    uint8_t type = status & 0xF0;
    uint8_t channel = (status & 0x0F) + 1;
    uint8_t d1 = pkt[2];
    uint8_t d2 = pkt[3];

    switch (cin) {
        case 0x9: /* Note on */
            if (d2 > 0) {
                ESP_LOGI(MIDI_TAG, "NoteOn  ch=%d note=%d vel=%d", channel, d1, d2);
            } else {
                ESP_LOGI(MIDI_TAG, "NoteOff ch=%d note=%d", channel, d1);
            }
            break;
        case 0x8: /* Note off */
            ESP_LOGI(MIDI_TAG, "NoteOff ch=%d note=%d vel=%d", channel, d1, d2);
            break;
        case 0xB: /* Control change */
            ESP_LOGI(MIDI_TAG, "CC      ch=%d cc=%d val=%d", channel, d1, d2);
            break;
        case 0xE: /* Pitch bend */
            {
                int bend = ((int)d2 << 7 | d1) - 8192;
                ESP_LOGI(MIDI_TAG, "Pitch   ch=%d bend=%d", channel, bend);
            }
            break;
        default:
            ESP_LOGD(MIDI_TAG, "CIN=0x%X st=0x%02X d=%02X %02X", cin, status, d1, d2);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Transfer callback                                                  */
/* ------------------------------------------------------------------ */

static void midi_in_xfer_cb(usb_transfer_t* transfer)
{
    midi_dev_t* m = &s_midi;
    if (!m->running) {
        return;
    }
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        /* USB-MIDI event packets are 4 bytes each. */
        int pkt_count = transfer->actual_num_bytes / 4;
        for (int i = 0; i < pkt_count; i++) {
            midi_log_event(&transfer->data_buffer[i * 4]);
        }
        /* Resubmit for continuous reception. */
        transfer->num_bytes = MIDI_XFER_BUF_LEN;
        esp_err_t ret = usb_host_transfer_submit(transfer);
        if (ret != ESP_OK) {
            ESP_LOGE(MIDI_TAG, "resubmit failed: %s", esp_err_to_name(ret));
            m->running = false;
        }
        return;
    }
    ESP_LOGW(MIDI_TAG, "IN transfer status=%d", transfer->status);
    m->running = false;
}

/* ------------------------------------------------------------------ */
/* Attach / detach                                                    */
/* ------------------------------------------------------------------ */

bool midi_host_init(usb_host_client_handle_t client)
{
    s_client = client;
    return s_client != NULL;
}

bool midi_host_attach(usb_device_handle_t dev_hdl, const usb_config_desc_t* cfg_desc)
{
    if (!s_client) {
        return false;
    }
    if (s_midi.running) {
        ESP_LOGW(MIDI_TAG, "MIDI already attached (single device supported)");
        return false;
    }
    memset(&s_midi, 0, sizeof(s_midi));
    s_midi.dev_hdl = dev_hdl;

    /* Find the MIDI streaming interface (class 0x01, subclass 0x03). */
    int midi_if_num = -1;
    const usb_intf_desc_t* midi_ifc = NULL;
    int offset = 0;
    for (int i = 0; i < cfg_desc->bNumInterfaces; i++) {
        const usb_intf_desc_t* ifc =
            usb_parse_interface_descriptor(cfg_desc, i, 0, &offset);
        if (ifc == NULL) {
            continue;
        }
        if (ifc->bInterfaceClass == 0x01 && ifc->bInterfaceSubClass == 0x03) {
            midi_if_num = i;
            midi_ifc = ifc;
            break;
        }
    }
    if (midi_if_num < 0 || midi_ifc == NULL) {
        ESP_LOGW(MIDI_TAG, "No MIDI streaming interface on this device");
        return false;
    }

    esp_err_t ret = usb_host_interface_claim(s_client, dev_hdl, midi_if_num, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(MIDI_TAG, "interface claim failed: %s", esp_err_to_name(ret));
        return false;
    }
    s_midi.intf_num = midi_if_num;
    s_midi.claimed = true;

    /* Locate the bulk IN endpoint of the MIDI interface. */
    const usb_ep_desc_t* in_ep = NULL;
    int ep_offset = 0;
    for (int e = 0; e < midi_ifc->bNumEndpoints; e++) {
        const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
            midi_ifc, e, cfg_desc->wTotalLength, &ep_offset);
        if (ep == NULL) {
            continue;
        }
        if ((ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
            && USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK) {
            in_ep = ep;
            break;
        }
    }
    if (in_ep == NULL) {
        ESP_LOGE(MIDI_TAG, "No bulk IN endpoint on MIDI interface");
        usb_host_interface_release(s_client, dev_hdl, midi_if_num);
        s_midi.claimed = false;
        return false;
    }

    ret = usb_host_transfer_alloc(MIDI_XFER_BUF_LEN, 0, &s_midi.in_xfer);
    if (ret != ESP_OK) {
        ESP_LOGE(MIDI_TAG, "transfer alloc failed: %s", esp_err_to_name(ret));
        usb_host_interface_release(s_client, dev_hdl, midi_if_num);
        s_midi.claimed = false;
        return false;
    }
    s_midi.in_xfer->device_handle = dev_hdl;
    s_midi.in_xfer->bEndpointAddress = in_ep->bEndpointAddress;
    s_midi.in_xfer->num_bytes = MIDI_XFER_BUF_LEN;
    s_midi.in_xfer->callback = midi_in_xfer_cb;
    s_midi.in_xfer->context = NULL;

    ret = usb_host_transfer_submit(s_midi.in_xfer);
    if (ret != ESP_OK) {
        ESP_LOGE(MIDI_TAG, "transfer submit failed: %s", esp_err_to_name(ret));
        usb_host_transfer_free(s_midi.in_xfer);
        s_midi.in_xfer = NULL;
        usb_host_interface_release(s_client, dev_hdl, midi_if_num);
        s_midi.claimed = false;
        return false;
    }

    s_midi.running = true;
    ESP_LOGI(MIDI_TAG, "MIDI attached: ifc %d, IN EP 0x%02X, mps %d",
             midi_if_num, in_ep->bEndpointAddress, in_ep->wMaxPacketSize);
    return true;
}

void midi_host_detach(usb_device_handle_t dev_hdl)
{
    if (s_midi.dev_hdl != dev_hdl) {
        return;
    }
    s_midi.running = false;
    if (s_midi.in_xfer) {
        usb_host_transfer_free(s_midi.in_xfer);
        s_midi.in_xfer = NULL;
    }
    if (s_midi.claimed) {
        usb_host_interface_release(s_client, dev_hdl, s_midi.intf_num);
        s_midi.claimed = false;
    }
    ESP_LOGI(MIDI_TAG, "MIDI detached");
}
