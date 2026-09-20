/**
 * @file bsp_board_config.h
 * @brief Board configuration for Guition JC1060P470C_I_W_Y
 * 
 * Hardware specifications:
 * - Display: JD9165 MIPI-DSI, 1024×600, 51.2MHz pixel clock
 * - Touch: GT911 I2C (0x5D)
 * - Audio: ES8311 codec
 * - ESP32-P4 with ESP32-C6 Wi-Fi co-processor
 */

#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// BOARD IDENTIFICATION
// =============================================================================

#define BSP_BOARD_NAME          "JC1060P470C_I_W_Y"
#define BSP_BOARD_VENDOR        "Guition"
#define BSP_DISPLAY_NAME        "JD9165_1024x600"

// =============================================================================
// DISPLAY - JD9165 MIPI-DSI
// =============================================================================

/** @brief Display resolution (landscape orientation) */
#define BSP_LCD_H_RES           (1024)
#define BSP_LCD_V_RES           (600)

/** @brief MIPI-DSI configuration */
#define BSP_LCD_MIPI_DSI_LANE_NUM        (2)           // 2 data lanes
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS (1000)      // 1.0 Gbps per lane (proven value for this panel)
#define BSP_LCD_PIXEL_CLOCK_HZ           (51200000)    // 51.2 MHz

/** @brief Display timing parameters (from JD9165 datasheet) */
#define BSP_LCD_H_SYNC            (24)     // HSYNC pulse width
#define BSP_LCD_H_BACK_PORCH      (136)    // Horizontal back porch
#define BSP_LCD_H_FRONT_PORCH     (160)    // Horizontal front porch
#define BSP_LCD_V_SYNC            (2)      // VSYNC pulse width
#define BSP_LCD_V_BACK_PORCH      (21)     // Vertical back porch
#define BSP_LCD_V_FRONT_PORCH     (12)     // Vertical front porch

/** @brief Total frame timing (for refresh rate calculation) */
#define BSP_LCD_H_TOTAL           (BSP_LCD_H_RES + BSP_LCD_H_SYNC + BSP_LCD_H_BACK_PORCH + BSP_LCD_H_FRONT_PORCH)
#define BSP_LCD_V_TOTAL           (BSP_LCD_V_RES + BSP_LCD_V_SYNC + BSP_LCD_V_BACK_PORCH + BSP_LCD_V_FRONT_PORCH)
// Refresh rate ≈ 51.2MHz / (1320 × 635) ≈ 60.8 Hz

/** @brief GPIO pins for display */
#define BSP_LCD_RST_GPIO          (GPIO_NUM_5)
#define BSP_LCD_BL_GPIO           (GPIO_NUM_23)
#define BSP_LCD_TE_GPIO           (GPIO_NUM_NC)   // Tearing Effect signal (not connected)

/** @brief Display color format */
#define BSP_LCD_COLOR_BITS        (16)            // RGB565 - vendor LVGL9 baseline (guition-esp32p4-lvgl9)

/** @brief Buffer configuration */
#define BSP_LCD_DRAW_BUFF_DOUBLE  (1)             // Double buffering
#define BSP_LCD_DRAW_BUFF_SIZE    (BSP_LCD_H_RES * 100)  // Partial buffer for DMA

/** @brief Framebuffer count (1 = single buffer, no tear-free swap yet) */
#define BSP_LCD_FRAMEBUFFER_COUNT (1)

// =============================================================================
// TOUCH SCREEN - GT911
// =============================================================================

/** @brief I2C configuration for touch
 *
 *  Shared hardware I2C master on GPIO7 (SDA) / GPIO8 (SCL) with GT911 and
 *  ES8311 (per the official board pin map / ESPHome example). The board has
 *  external pull-ups; internal pull-ups are not required.
 */
#define BSP_TOUCH_I2C_PORT        (I2C_NUM_0)
#define BSP_TOUCH_I2C_SDA_GPIO    (GPIO_NUM_7)
#define BSP_TOUCH_I2C_SCL_GPIO    (GPIO_NUM_8)
#define BSP_TOUCH_I2C_CLK_SPEED_HZ (400000)       // 400 kHz

/** @brief GT911 configuration
 *
 *  The reference firmware does not wire GT911 RST/INT to software GPIOs
 *  (power-on default address 0x5D). Do NOT drive GPIO7/8 as RST/INT - they
 *  are the I2C bus lines themselves.
 */
#define BSP_TOUCH_I2C_ADDR        (0x5D)          // GT911 I2C address
#define BSP_TOUCH_RST_GPIO        (GPIO_NUM_NC)
#define BSP_TOUCH_INT_GPIO        (GPIO_NUM_NC)

/** @brief Touch resolution (matches display) */
#define BSP_TOUCH_X_MAX           (BSP_LCD_H_RES)
#define BSP_TOUCH_Y_MAX           (BSP_LCD_V_RES)

/** @brief Touch transformation (landscape, no rotation needed) */
#define BSP_TOUCH_SWAP_XY         (0)
#define BSP_TOUCH_MIRROR_X        (0)
#define BSP_TOUCH_MIRROR_Y        (0)

// =============================================================================
// AUDIO - ES8311 Codec
// =============================================================================

/** @brief I2C configuration for audio codec
 *
 *  ES8311 shares the touch I2C master bus (GPIO7 SDA / GPIO8 SCL);
 *  BSP_AUDIO_I2C_PORT is unused for control (bus_handle is passed instead).
 */
#define BSP_AUDIO_I2C_PORT        (I2C_NUM_0)     // Shared with touch (via bus_handle)
#define BSP_AUDIO_I2C_SDA_GPIO    (GPIO_NUM_7)    // Shared bus (informational)
#define BSP_AUDIO_I2C_SCL_GPIO    (GPIO_NUM_8)    // Shared bus (informational)
#define BSP_AUDIO_I2C_CLK_SPEED_HZ (400000)

/** @brief ES8311 configuration */
/** @brief ES8311 configuration
 *
 *  esp_codec_dev expects the address in 8-bit form (it shifts right by 1
 *  internally); 0x30 == 7-bit 0x18. Passing the 7-bit form targets 0x0C.
 */
#define BSP_AUDIO_CODEC_ADDR      (0x30)          // ES8311 (esp_codec_dev 8-bit form; 7-bit = 0x18)
#define BSP_AUDIO_PA_CTRL_GPIO    (GPIO_NUM_11)   // NS4150 PA enable (PA-CTRL)

/** @brief I2S configuration (official pin map: BCLK=12, WS=10, MCLK=13, DOUT=9, DIN=48) */
#define BSP_AUDIO_I2S_PORT        (I2S_NUM_0)
#define BSP_AUDIO_I2S_SCLK_GPIO   (GPIO_NUM_12)   // BCLK
#define BSP_AUDIO_I2S_LRCK_GPIO   (GPIO_NUM_10)   // WS / LRCK
#define BSP_AUDIO_I2S_DOUT_GPIO   (GPIO_NUM_9)
#define BSP_AUDIO_I2S_DIN_GPIO    (GPIO_NUM_48)   // Microphone input
#define BSP_AUDIO_I2S_MCLK_GPIO   (GPIO_NUM_13)   // Required by ES8311 (use_mclk=true)

/** @brief Audio sample rates */
#define BSP_AUDIO_SAMPLE_RATE     (44100)
#define BSP_AUDIO_BIT_WIDTH       (I2S_DATA_BIT_WIDTH_16BIT)
#define BSP_AUDIO_CHANNEL_FORMAT  (I2S_SLOT_MODE_STEREO)

// =============================================================================
// I2C BUS PLAN (per official JC1060P470C_I_W_Y pin map)
// =============================================================================
//
// The real board wires GT911 AND ES8311 on a single I2C bus:
//   SDA = GPIO7, SCL = GPIO8 (external pull-ups on the board).
// There is NO GPIO12 conflict: GPIO12 is I2S BCLK and GPIO10 is I2S WS.
// GPIO14/15 are reserved for the ESP32-C6 SDIO (Wi-Fi) - never use them
// as I2C. We therefore use the regular hardware i2c_master driver on 7/8.
//
// NOTE: GPIO14/15 are SDIO D0/D1 of the C6; the I2S1 "alternative pins"
// below are likewise NOT free (14-17 = C6 SDIO).

#define BSP_USE_SW_I2C_FOR_TOUCH  (1)   // Shared I2C master created in bsp_board_init()
#define BSP_USE_SW_I2C_FOR_AUDIO  (1)   // Touch + audio share the same master bus

// Kept for API compatibility with bsp_i2c_init_sw() call sites:
#define BSP_I2C_SW_SDA_GPIO       (GPIO_NUM_7)
#define BSP_I2C_SW_SCL_GPIO       (GPIO_NUM_8)

// =============================================================================
// USB HOST (DDJ-400 controller + USB mass storage)
// =============================================================================
//
// The P4 has TWO OTG controllers (SOC_USB_OTG_PERIPH_NUM=2):
//   - OTG on the internal UTMI PHY  -> HS port  (480 Mbps)
//   - OTG on the internal FSLS PHY  -> FS port  (12 Mbps)
// On the JC1060 both reach a USB-C receptacle ("High Speed USB" and "Full
// Speed USB" schematic blocks). The USB D+/D- pads are dedicated: no GPIO
// configuration needed (the old BSP_USB_DM/DP GPIO defines below are only
// kept as placeholders and must not be trusted).
// The FSLS PHY is muxed with USB-Serial-JTAG: the FS USB-C is currently our
// flashing/console port (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y). Using it as
// host would kill the console.
// Plan (see BRING_UP_GUIDE): powered hub on the HS USB-C for DDJ-400 + MSC,
// FS USB-C stays the console. Host stack on IDF 6.0.2 = espressif/usb_host_lib
// managed component; hub support = CONFIG_USB_HOST_HUBS_SUPPORTED.
//
// Legacy placeholders (WRONG on this board - GPIO19 is the C6 SDIO CMD):
#define BSP_USB_HOST_ENABLED      (0)
#define BSP_USB_DM_GPIO           (GPIO_NUM_NC)
#define BSP_USB_DP_GPIO           (GPIO_NUM_NC)

// =============================================================================
// SDMMC (for config/cache, optional)
// =============================================================================

#define BSP_SDMMC_ENABLED         (1)
#define BSP_SDMMC_D0_GPIO         (GPIO_NUM_39)
#define BSP_SDMMC_D1_GPIO         (GPIO_NUM_40)
#define BSP_SDMMC_D2_GPIO         (GPIO_NUM_41)
#define BSP_SDMMC_D3_GPIO         (GPIO_NUM_42)
#define BSP_SDMMC_CMD_GPIO        (GPIO_NUM_44)
#define BSP_SDMMC_CLK_GPIO        (GPIO_NUM_43)
#define BSP_SDMMC_SLOT            (SDMMC_HOST_SLOT_0)

// =============================================================================
// UART CONTROL LINK (to ESP32-S3 control board)
// =============================================================================

#define BSP_UART_CONTROL_LINK     (UART_NUM_1)
// JC1060P470C: GPIO28/29 are consumed by the Ethernet RMII (IP101) and are
// not on any connector. Use GPIO45/46 exposed on the 2x10 2.54 mm expansion
// header (JP1 area of the Expand IO page): pin 10 = GPIO45, pin 9 = GPIO46.
#define BSP_UART_TX_GPIO          (GPIO_NUM_45)   // To S3 RX (GPIO6)
#define BSP_UART_RX_GPIO          (GPIO_NUM_46)   // From S3 TX (GPIO5)
#define BSP_UART_BAUDRATE         (460800)

// =============================================================================
// ESP-Hosted (ESP32-C6 Wi-Fi co-processor)
// =============================================================================

#define BSP_ESP_HOSTED_ENABLED    (1)
#define BSP_ESP_HOSTED_SDIO_CLK   (GPIO_NUM_NC)   // TODO: Verify from schematic
#define BSP_ESP_HOSTED_SDIO_CMD   (GPIO_NUM_NC)
#define BSP_ESP_HOSTED_SDIO_D0    (GPIO_NUM_NC)
#define BSP_ESP_HOSTED_SDIO_D1    (GPIO_NUM_NC)
#define BSP_ESP_HOSTED_SDIO_D2    (GPIO_NUM_NC)
#define BSP_ESP_HOSTED_SDIO_D3    (GPIO_NUM_NC)
#define BSP_ESP_HOSTED_RST_GPIO   (GPIO_NUM_54)   // C6 reset
#define BSP_ESP_HOSTED_WAKE_GPIO  (GPIO_NUM_NC)

// =============================================================================
// USER BUTTONS / LEDs (if any)
// =============================================================================

#define BSP_USER_BUTTON_GPIO      (GPIO_NUM_NC)   // No user button documented
#define BSP_USER_LED_GPIO         (GPIO_NUM_NC)   // No user LED documented

// =============================================================================
// POWER MANAGEMENT
// =============================================================================

#define BSP_BATTERY_ENABLED       (0)   // Board is USB-powered only
#define BSP_ADC_ENABLED           (0)   // No ADC usage documented

#ifdef __cplusplus
}
#endif
