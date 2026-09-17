/**
 * @file bsp_display.c
 * @brief Display driver for JC1060P470C_I_W_Y (JD9165 MIPI-DSI)
 * 
 * Based on Guition demo code and ESP-IDF MIPI-DSI framework
 */

#include "bsp_board_config.h"
#include "bsp/display.h"
#include "bsp_common.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_jd9165.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_private/esp_clk.h"
#include "esp_heap_caps.h"

#if LVGL_VERSION_MAJOR >= 9
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

static const char* TAG = "bsp_display";

/** @brief JD9165 initialization sequence (from Guition demo) */
static const jd9165_lcd_init_cmd_t jd9165_init_cmds[] = {
    // {command, {data}, data_size, delay_ms}
    {0x30, {0x00}, 1, 0},
    {0xF7, {0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, {0x01}, 1, 0},
    {0x04, {0x0C}, 1, 0},
    {0x05, {0x00}, 1, 0},  // HBP adjustment
    {0x06, {0x00}, 1, 0},  // VBP adjustment
    {0x0B, {0x11}, 1, 0},  // 2 lanes (0x11), 1 lane would be 0x10
    {0x17, {0x00}, 1, 0},
    {0x20, {0x04}, 1, 0},  // Lane select
    {0x1F, {0x05}, 1, 0},  // HS settle time
    {0x23, {0x00}, 1, 0},  // Close GAS
    {0x25, {0x19}, 1, 0},
    {0x28, {0x18}, 1, 0},
    {0x29, {0x04}, 1, 0},  // VCOM
    {0x2A, {0x01}, 1, 0},  // VCOM
    {0x2B, {0x04}, 1, 0},  // VCOM
    {0x2C, {0x01}, 1, 0},  // VCOM
    {0x30, {0x02}, 1, 0},
    {0x01, {0x22}, 1, 0},
    {0x03, {0x12}, 1, 0},
    {0x04, {0x00}, 1, 0},
    {0x05, {0x64}, 1, 0},
    {0x0A, {0x08}, 1, 0},
    {0x0B, {0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 11, 0},
    {0x0C, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, {0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 11, 0},
    {0x0E, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, {0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 11, 0},
    {0x10, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, {0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 11, 0},
    {0x12, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x14, {0x00, 0x00, 0x11, 0x11}, 4, 0},  // CKV_OFF timing
    {0x18, {0x99}, 1, 0},
    {0x30, {0x06}, 1, 0},
    {0x12, {0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x13, {0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x30, {0x0A}, 1, 0},
    {0x02, {0x4F}, 1, 0},
    {0x0B, {0x40}, 1, 0},
    {0x12, {0x3E}, 1, 0},
    {0x13, {0x78}, 1, 0},
    {0x30, {0x0D}, 1, 0},
    {0x0D, {0x04}, 1, 0},
    {0x10, {0x0C}, 1, 0},
    {0x11, {0x0C}, 1, 0},
    {0x12, {0x0C}, 1, 0},
    {0x13, {0x0C}, 1, 0},
    {0x30, {0x00}, 1, 0},
    {0x11, {0}, 0, 120},  // SLPOUT with 120ms delay
    {0x29, {0}, 0, 20},   // DISPON with 20ms delay
    {REGFLAG_END_OF_TABLE, {0}, 0, 0}
};

static lv_display_t* g_disp = NULL;
static lv_indev_t* g_indev = NULL;
static esp_lcd_panel_handle_t g_panel = NULL;

/**
 * @brief Enable power for MIPI DSI PHY
 */
static esp_err_t bsp_enable_dsi_phy_power(void)
{
    // MIPI DSI PHY powered by LDO VO3 (2.5V)
    // In production, this should be controlled by PMIC
    // For now, assume always-on from USB power
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (assumed always-on)");
    return ESP_OK;
}

/**
 * @brief Initialize MIPI DSI bus
 */
static esp_err_t bsp_dsi_bus_init(esp_lcd_dsi_bus_handle_t* mipi_dsi_bus)
{
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, mipi_dsi_bus), 
                        TAG, "Failed to create MIPI-DSI bus");
    
    ESP_LOGI(TAG, "MIPI-DSI bus created: %d lanes @ %d Mbps", 
             BSP_LCD_MIPI_DSI_LANE_NUM, BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS);
    
    return ESP_OK;
}

/**
 * @brief Initialize LCD panel IO (DBI interface for commands)
 */
static esp_err_t bsp_lcd_io_init(esp_lcd_dsi_bus_handle_t mipi_dsi_bus,
                                  esp_lcd_panel_io_handle_t* io)
{
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, io),
                        TAG, "Failed to create DBI panel IO");
    
    ESP_LOGD(TAG, "DBI panel IO created");
    return ESP_OK;
}

/**
 * @brief Initialize JD9165 panel
 */
static esp_err_t bsp_lcd_panel_init(esp_lcd_panel_io_handle_t io,
                                     esp_lcd_panel_handle_t* panel)
{
    // MIPI DPI configuration
    esp_lcd_dpi_panel_config_t dpi_config = {
        .pixel_clock_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .timing = {
            .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
            .h_res = BSP_LCD_H_RES,
            .v_res = BSP_LCD_V_RES,
            .hsync_pulse_width_cycles = BSP_LCD_H_SYNC,
            .hback_porch = BSP_LCD_H_BACK_PORCH,
            .hfront_porch = BSP_LCD_H_FRONT_PORCH,
            .vsync_pulse_width_lines = BSP_LCD_V_SYNC,
            .vback_porch = BSP_LCD_V_BACK_PORCH,
            .vfront_porch = BSP_LCD_V_FRONT_PORCH,
            .flags = {
                .hsync_idle_low = false,
                .vsync_idle_low = false,
                .de_idle_low = false,
                .pclk_active_neg = false,
                .pclk_idle_high = false,
            },
        },
        .num_fbs = BSP_LCD_FRAMEBUFFER_COUNT,
        .flags.fb_in_psram = true,  // Allocate framebuffers in PSRAM
    };

    // Vendor-specific configuration
    jd9165_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = NULL,  // Will be set by panel driver
            .dpi_config = &dpi_config,
        },
        .init_cmds = jd9165_init_cmds,
        .init_cmds_size = sizeof(jd9165_init_cmds) / sizeof(jd9165_lcd_init_cmd_t),
        .flags = {
            .use_mipi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = BSP_LCD_RST_GPIO,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = BSP_LCD_COLOR_BITS,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(io, &lcd_dev_config, panel),
                        TAG, "Failed to create JD9165 panel");
    
    ESP_LOGI(TAG, "JD9165 panel created");
    
    // Reset panel
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "Panel reset failed");
    
    // Initialize panel
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "Panel init failed");
    
    // Turn on display
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), 
                        TAG, "Failed to turn on display");
    
    ESP_LOGI(TAG, "Display initialized: %dx%d @ %dHz", 
             BSP_LCD_H_RES, BSP_LCD_V_RES, 
             BSP_LCD_PIXEL_CLOCK_HZ / (BSP_LCD_H_TOTAL * BSP_LCD_V_TOTAL));
    
    return ESP_OK;
}

esp_err_t bsp_display_new(const bsp_display_cfg_t* config,
                          esp_lcd_panel_handle_t* ret_panel,
                          esp_lcd_panel_io_handle_t* ret_io)
{
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    ESP_RETURN_ON_ERROR(bsp_enable_dsi_phy_power(), TAG, "DSI PHY power failed");
    ESP_RETURN_ON_ERROR(bsp_dsi_bus_init(&mipi_dsi_bus), TAG, "DSI bus init failed");
    ESP_RETURN_ON_ERROR(bsp_lcd_io_init(mipi_dsi_bus, &io), TAG, "LCD IO init failed");
    ESP_RETURN_ON_ERROR(bsp_lcd_panel_init(io, &panel), TAG, "LCD panel init failed");

    if (ret_panel) *ret_panel = panel;
    if (ret_io) *ret_io = io;

    return ESP_OK;
}

esp_err_t bsp_display_new_with_handles(const bsp_display_cfg_t* config,
                                        bsp_lcd_handles_t* ret_handles)
{
    esp_err_t ret;
    
    ret = bsp_display_new(config, &ret_handles->panel, &ret_handles->io);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // DSI bus handle stored internally by panel driver
    ret_handles->mipi_dsi_bus = NULL;  // Not directly accessible
    ret_handles->control = NULL;
    
    return ESP_OK;
}

esp_err_t bsp_display_brightness_init(void)
{
    // Already initialized in bsp_board_init()
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    return bsp_backlight_set_brightness((uint8_t)brightness_percent);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_backlight_on();
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_backlight_off();
}

#if LVGL_VERSION_MAJOR >= 9
static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
#else
static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px_map)
#endif
{
    if (g_panel == NULL) {
        return;
    }

    int x1 = area->x1;
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;

    // Draw bitmap to display
    esp_lcd_panel_draw_bitmap(g_panel, x1, y1, x2 + 1, y2 + 1, px_map);

    // Notify LVGL that flushing is done
#if LVGL_VERSION_MAJOR >= 9
    lv_display_flush_ready(disp);
#else
    lv_disp_flush_ready(drv);
#endif
}

lv_display_t* bsp_display_start_with_config(const bsp_display_cfg_t* cfg)
{
    if (g_disp != NULL) {
        ESP_LOGW(TAG, "Display already initialized");
        return g_disp;
    }

    ESP_LOGI(TAG, "Starting display with config: %dx%d, buffer=%d",
             cfg->h_res, cfg->v_res, cfg->buffer_size);

    // Initialize display hardware
    bsp_lcd_handles_t lcd_handles;
    esp_err_t ret = bsp_display_new_with_handles(cfg, &lcd_handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return NULL;
    }

    g_panel = lcd_handles.panel;

    // Initialize LVGL
    lv_init();

    // Create LVGL display
    g_disp = lv_display_create(cfg->h_res, cfg->v_res);
    if (g_disp == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return NULL;
    }

    // Allocate draw buffers
    void* buf1 = NULL;
    void* buf2 = cfg->double_buffer ? NULL : NULL;
    size_t buffer_bytes = cfg->buffer_size * sizeof(lv_color_t);

    uint32_t caps = MALLOC_CAP_SPIRAM;
    if (cfg->flags.buff_dma) {
        caps |= MALLOC_CAP_DMA;
    }

    buf1 = heap_caps_malloc(buffer_bytes, caps);
    if (buf1 == NULL) {
        ESP_LOGW(TAG, "Failed to allocate buffer in SPIRAM, trying DMA");
        buf1 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA);
    }
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate draw buffer");
        lv_display_delete(g_disp);
        return NULL;
    }

    ESP_LOGI(TAG, "Draw buffer allocated: %d bytes @ %p", buffer_bytes, buf1);

    if (cfg->double_buffer) {
        buf2 = heap_caps_malloc(buffer_bytes, caps);
        if (buf2 == NULL) {
            ESP_LOGW(TAG, "Double buffer allocation failed, using single buffer");
        }
    }

    lv_display_set_buffers(g_disp, buf1, buf2, buffer_bytes,
                           cfg->double_buffer && buf2 ? 
                               LV_DISPLAY_RENDER_MODE_FULL : 
                               LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_display_set_flush_cb(g_disp, lvgl_flush_cb);
    lv_display_set_user_data(g_disp, g_panel);

    // No rotation needed (native landscape)
    // lv_display_set_rotation(g_disp, LV_DISPLAY_ROTATION_0);

    ESP_LOGI(TAG, "LVGL display initialized");

    return g_disp;
}

lv_display_t* bsp_display_start(void)
{
    static const bsp_display_cfg_t default_cfg = {
        .h_res = BSP_LCD_H_RES,
        .v_res = BSP_LCD_V_RES,
        .bits_per_pixel = BSP_LCD_COLOR_BITS,
        .color_format = BSP_LCD_COLOR_FORMAT,
        .color_space = BSP_LCD_COLOR_SPACE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        },
    };

    return bsp_display_start_with_config(&default_cfg);
}

void bsp_display_rotate(lv_display_t* disp, lv_disp_rotation_t rotation)
{
    if (disp) {
        lv_disp_set_rotation(disp, rotation);
    }
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    // LVGL thread safety (if using mutex)
    // For now, assume single-threaded LVGL
    return true;
}

void bsp_display_unlock(void)
{
    // No-op for now
}

lv_indev_t* bsp_display_get_input_dev(void)
{
    return g_indev;
}

void bsp_display_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_map)
{
    lvgl_flush_cb(disp, area, color_map);
}
