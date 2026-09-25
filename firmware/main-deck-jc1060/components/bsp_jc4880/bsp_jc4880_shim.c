/**
 * @file bsp_jc4880_shim.c
 * @brief Upstream bsp_jc4880 API implemented on top of the JC1060P470 BSP.
 *
 * The upstream (dvucinozd) audio engine and UI talk to the board through the
 * bsp_jc4880.h surface. The JC1060P470C_I_W_Y board ships a different,
 * esp-bsp-style BSP (bsp_jc1060p470). This shim maps one onto the other so the
 * upstream components stay unmodified:
 *   - display: raw DPI panel (no esp_lvgl_port) -> bsp_display_get_panel_handle()
 *   - touch:   GT911 via bsp_touch_new()
 *   - audio:   ES8311 codec is NOT used (headphone cue goes to the DDJ over
 *              UAC, exactly like upstream). MAIN OUT is the I2S TX channel
 *              feeding the external PCM5102A, unless CONFIG_PAJ_MAIN_OUT_USB
 *              routes MAIN to the DDJ UAC path only (audio_engine always
 *              pushes master+phones to controller_usb_host_write_audio; the
 *              I2S handle is simply withheld when the USB sink is selected).
 *   - sd:      SDMMC mount wrapper.
 */
#include "bsp_jc4880.h"

#include <sys/statvfs.h>

#include "esp_log.h"
#include "driver/i2s_std.h"

/* The JC1060 BSP exposes bsp_audio_init(const bsp_audio_config_t*); the
 * upstream surface wants bsp_audio_init(void). Rename the BSP one locally. */
#define bsp_audio_init bsp_audio_init_cfg

#include "bsp/bsp_common.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "bsp/audio.h"
#include "bsp/sd.h"
#include "app_settings.h"

#undef bsp_audio_init

static const char *TAG = "bsp_jc4880";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static bool s_audio_up;
static bool s_sd_up;
static bsp_audio_out_t s_audio_out = BSP_AUDIO_OUT_RCA;
static bsp_monitor_route_t s_monitor_route = BSP_MONITOR_ROUTE_HEADPHONES;
static bool s_speaker_pa;
static uint32_t s_main_rate = 44100;

esp_err_t bsp_display_init(void)
{
    /* Board-level init (power, I2C) is idempotent inside the JC1060 BSP. */
    (void)bsp_board_init();

    bsp_display_cfg_t cfg = {
        .h_res = BSP_LCD_H_RES,
        .v_res = BSP_LCD_V_RES,
        .bits_per_pixel = 24,
        .double_buffer = false,
        .buffer_size = BSP_LCD_H_RES * 50,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t rc = bsp_display_new(&cfg, &s_panel, &io);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "display init: %s", esp_err_to_name(rc));
        return rc;
    }
    return ESP_OK;
}

void bsp_display_set_backlight(uint8_t pct)
{
    bsp_backlight_set_brightness(pct);
    bsp_backlight_on();
}

esp_err_t bsp_touch_init(void)
{
    bsp_touch_config_t cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .swap_xy = false,
        .mirror_x = false,
        .mirror_y = false,
    };
    return bsp_touch_new(&cfg, &s_touch);
}

esp_lcd_panel_handle_t bsp_display_get_panel_handle(void)
{
    return s_panel;
}

esp_lcd_touch_handle_t bsp_touch_get_handle(void)
{
    return s_touch;
}

i2c_master_bus_handle_t bsp_get_i2c_bus(void)
{
    return bsp_i2c_get_shared();
}

/* ES8311 onboard codec deliberately not brought up: headphones play through
 * the DDJ-FLX4 UAC path, MAIN through the PCM5102A (or UAC when toggled). */
esp_codec_dev_handle_t bsp_audio_get_codec_dev(void)
{
    return NULL;
}

esp_err_t bsp_audio_force_safe_boot_state(void)
{
    s_speaker_pa = false;
    return ESP_OK;
}

esp_err_t bsp_audio_init(void)
{
    bsp_audio_config_t cfg = {
        .sample_rate = 44100,
        .bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .channel_format = I2S_SLOT_MODE_STEREO,
        .volume = 0,
    };
    esp_err_t rc = bsp_audio_init_cfg(&cfg);
    if (rc == ESP_OK) {
        s_audio_up = true;
        /* The MAIN I2S channel is enabled by bsp_audio_main_i2s_set_sample_
         * rate() when the engine opens the output — no enable here. */
    }
    return rc;
}

i2s_chan_handle_t bsp_audio_get_main_i2s_tx(void)
{
    /* Runtime MAIN sink selection (Settings toggle, persisted in NVS via
     * app_settings): 1 = DDJ UAC (withhold the I2S handle so the engine
     * skips the PCM5102A path), 0 = PCM5102A RCA MAIN OUT. */
    if (app_settings_get().main_out_usb) {
        return NULL;
    }
    if (!s_audio_up) {
        return NULL;
    }
    return bsp_audio_get_i2s_tx_chan();
}

esp_err_t bsp_audio_main_i2s_set_sample_rate(uint32_t sample_rate)
{
    i2s_chan_handle_t chan = bsp_audio_get_main_i2s_tx();
    if (!chan) {
        return ESP_OK;
    }
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    s_main_rate = sample_rate;
    (void)i2s_channel_disable(chan);
    esp_err_t rc = i2s_channel_reconfig_std_clock(chan, &clk);
    if (rc == ESP_OK) {
        rc = i2s_channel_enable(chan);
    }
    return rc;
}

esp_err_t bsp_audio_main_i2s_abort_write(void)
{
    i2s_chan_handle_t chan = bsp_audio_get_main_i2s_tx();
    if (!chan) {
        return ESP_OK;
    }
    (void)i2s_channel_disable(chan);
    return i2s_channel_enable(chan);
}

esp_err_t bsp_audio_set_output(bsp_audio_out_t out)
{
    s_audio_out = out;
    return ESP_OK;
}

bsp_audio_out_t bsp_audio_get_output(void)
{
    return s_audio_out;
}

esp_err_t bsp_audio_set_monitor_route(bsp_monitor_route_t route)
{
    s_monitor_route = route;
    return ESP_OK;
}

bsp_monitor_route_t bsp_audio_get_monitor_route(void)
{
    return s_monitor_route;
}

esp_err_t bsp_audio_set_speaker_pa_enabled(bool enabled)
{
    s_speaker_pa = enabled; /* no onboard PA on JC1060; state only */
    return ESP_OK;
}

bool bsp_audio_get_speaker_pa_enabled(void)
{
    return s_speaker_pa;
}

#define BSP_SD_MOUNT_ATTEMPTS 5

esp_err_t bsp_sd_init(void)
{
    /* Upstream's bsp_jc4880 retries the mount: SDMMC ACMD41 can surface a
     * spurious send_op_cond timeout (0x107) right after power-up. A single
     * attempt turns that transient into a fatal ESP_ERROR_CHECK abort in
     * app_main. Retry with a short settle delay like upstream does.
     * v233: bsp_sd_mount() releases the SDMMC slot on every failure and
     * retries at default speed; the settle grows per attempt (0.2..1.0 s).
     * The caller must treat a failure as "no SD", not abort. */
    sdmmc_card_t *card = NULL;
    esp_err_t rc = ESP_FAIL;
    for (int attempt = 1; attempt <= BSP_SD_MOUNT_ATTEMPTS; ++attempt) {
        rc = bsp_sd_mount("/sd", &card);
        if (rc == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGW("bsp_sd", "SD mounted on attempt %d/%d", attempt,
                         BSP_SD_MOUNT_ATTEMPTS);
            }
            break;
        }
        ESP_LOGW("bsp_sd", "SD mount attempt %d/%d failed (%s)", attempt,
                 BSP_SD_MOUNT_ATTEMPTS, esp_err_to_name(rc));
        if (attempt < BSP_SD_MOUNT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(200 * attempt));
        }
    }
    s_sd_up = (rc == ESP_OK);
    return rc;
}

bool bsp_sd_is_mounted(void)
{
    return s_sd_up;
}

esp_err_t bsp_sd_get_status(bsp_sd_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    out_status->mounted = s_sd_up;
    out_status->total_bytes = 0;
    out_status->free_bytes = 0;
    out_status->sector_size = 512;
    if (!s_sd_up) {
        return ESP_OK;
    }
    struct statvfs vfs;
    if (statvfs("/sd", &vfs) == 0) {
        out_status->total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        out_status->free_bytes = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        out_status->sector_size = (uint32_t)vfs.f_frsize;
    }
    return ESP_OK;
}
