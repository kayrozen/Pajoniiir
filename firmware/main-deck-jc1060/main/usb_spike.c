/*
 * usb_spike.c - v158 raw HPRT probe.
 *
 * Reads the DWC hprt registers of BOTH P4 USB controllers every 2 s and
 * logs prtconnsts/prtconndet. This bypasses every software stack: if the
 * connect bit never moves when a device is attached, the signal never
 * reaches the DWC (PHY/routing/board problem). If it moves, the attach
 * is seen by hardware and the problem is purely in the host-lib ISR/queue
 * path.
 */
#include "usb_spike.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/usb_dwc_struct.h"

static const char *TAG = "usb_spike";

static void hprt_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        volatile usb_dwc_hprt_reg_t hs = USB_DWC_HS.hprt_reg;
        volatile usb_dwc_hprt_reg_t fs = USB_DWC_FS.hprt_reg;
        ESP_LOGW(TAG,
                 "HPRT HS: conn=%d det=%d ena=%d pwr=%d | FS: conn=%d det=%d ena=%d pwr=%d",
                 hs.prtconnsts, hs.prtconndet, hs.prtena, hs.prtpwr,
                 fs.prtconnsts, fs.prtconndet, fs.prtena, fs.prtpwr);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void usb_spike_start(void)
{
    if (xTaskCreate(hprt_poll_task, "hprt_poll", 3072, NULL, 5, NULL)
        != pdPASS) {
        ESP_LOGE(TAG, "hprt poll task create failed");
    }
}
