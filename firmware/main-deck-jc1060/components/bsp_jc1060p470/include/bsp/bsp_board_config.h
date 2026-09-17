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
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS (1000)      // 1.0 Gbps per lane
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
#define BSP_LCD_COLOR_BITS        (16)            // RGB565
#define BSP_LCD_COLOR_FORMAT      ESP_LCD_COLOR_FORMAT_RGB565
#define BSP_LCD_COLOR_SPACE       ESP_LCD_COLOR_SPACE_RGB

/** @brief Buffer configuration */
#define BSP_LCD_DRAW_BUFF_DOUBLE  (1)             // Double buffering
#define BSP_LCD_DRAW_BUFF_SIZE    (BSP_LCD_H_RES * 100)  // Partial buffer for DMA

/** @brief Framebuffer count (1 = single buffer, no tear-free swap yet) */
#define BSP_LCD_FRAMEBUFFER_COUNT (1)

// =============================================================================
// TOUCH SCREEN - GT911
// =============================================================================

/** @brief I2C configuration for touch */
#define BSP_TOUCH_I2C_PORT        (I2C_NUM_1)
#define BSP_TOUCH_I2C_SDA_GPIO    (GPIO_NUM_12)
#define BSP_TOUCH_I2C_SCL_GPIO    (GPIO_NUM_10)
#define BSP_TOUCH_I2C_CLK_SPEED_HZ (400000)       // 400 kHz

/** @brief GT911 configuration */
#define BSP_TOUCH_I2C_ADDR        (0x5D)          // GT911 I2C address
#define BSP_TOUCH_RST_GPIO        (GPIO_NUM_8)
#define BSP_TOUCH_INT_GPIO        (GPIO_NUM_7)

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

/** @brief I2C configuration for audio codec */
#define BSP_AUDIO_I2C_PORT        (I2C_NUM_0)     // Separate I2C for audio
#define BSP_AUDIO_I2C_SDA_GPIO    (GPIO_NUM_7)    // Shared with touch SDA
#define BSP_AUDIO_I2C_SCL_GPIO    (GPIO_NUM_8)    // Shared with touch SCL
#define BSP_AUDIO_I2C_CLK_SPEED_HZ (400000)

/** @brief ES8311 configuration */
#define BSP_AUDIO_CODEC_ADDR      (0x18)          // ES8311 I2C address (default)
#define BSP_AUDIO_PA_CTRL_GPIO    (GPIO_NUM_1)    // NS4150 PA enable

/** @brief I2S configuration */
#define BSP_AUDIO_I2S_PORT        (I2S_NUM_0)
#define BSP_AUDIO_I2S_SCLK_GPIO   (GPIO_NUM_13)
#define BSP_AUDIO_I2S_LRCK_GPIO   (GPIO_NUM_12)   // ⚠️ CONFLICT: Same as touch SDA!
#define BSP_AUDIO_I2S_DOUT_GPIO   (GPIO_NUM_9)
#define BSP_AUDIO_I2S_DIN_GPIO    (GPIO_NUM_NC)   // Not used (playback only)
#define BSP_AUDIO_I2S_MCLK_GPIO   (GPIO_NUM_NC)   // Not required by ES8311

/** @brief Audio sample rates */
#define BSP_AUDIO_SAMPLE_RATE     (44100)
#define BSP_AUDIO_BIT_WIDTH       (I2S_DATA_BIT_WIDTH_16BIT)
#define BSP_AUDIO_CHANNEL_FORMAT  (I2S_SLOT_MODE_STEREO)

// =============================================================================
// ⚠️ GPIO CONFLICT RESOLUTION
// =============================================================================

/**
 * @brief CRITICAL: GPIO12 conflict between I2S LRCK and I2C SDA
 * 
 * The JC1060P470C schematic shows:
 * - GPIO12: I2S LRCK (audio)
 * - GPIO12: Also routed to I2C SDA (touch/codec)
 * 
 * This is a HARDWARE DESIGN ISSUE. Solutions:
 * 
 * OPTION 1 (Recommended): Use I2S1 for audio instead of I2S0
 *   - I2S0: Keep for other peripherals if needed
 *   - I2S1: Use GPIOs 14-17 for audio (check availability)
 * 
 * OPTION 2: Bit-bang I2C on different GPIOs
 *   - Use GPIO14/15 for I2C (software I2C)
 *   - Keep hardware I2S on GPIO12
 * 
 * OPTION 3: Hardware modification (not recommended)
 *   - Cut trace and jumper to different GPIO
 * 
 * For this implementation, we'll use OPTION 2 (software I2C for touch/codec).
 */

#define BSP_USE_SW_I2C_FOR_TOUCH  (1)   // Use software I2C for touch
#define BSP_USE_SW_I2C_FOR_AUDIO  (1)   // Use software I2C for codec

// Alternative I2C pins for software bit-banging
#define BSP_I2C_SW_SDA_GPIO       (GPIO_NUM_14)
#define BSP_I2C_SW_SCL_GPIO       (GPIO_NUM_15)

// Alternative I2S1 pins (if OPTION 1 is chosen)
#define BSP_AUDIO_I2S1_SCLK_GPIO  (GPIO_NUM_14)
#define BSP_AUDIO_I2S1_LRCK_GPIO  (GPIO_NUM_15)
#define BSP_AUDIO_I2S1_DOUT_GPIO  (GPIO_NUM_16)
#define BSP_AUDIO_I2S1_MCLK_GPIO  (GPIO_NUM_17)

// =============================================================================
// USB HOST (MSC for music library)
// =============================================================================

#define BSP_USB_HOST_ENABLED      (1)
#define BSP_USB_DM_GPIO           (GPIO_NUM_19)   // D+ (check schematic)
#define BSP_USB_DP_GPIO           (GPIO_NUM_20)   // D- (check schematic)

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
#define BSP_UART_TX_GPIO          (GPIO_NUM_28)   // To S3 RX
#define BSP_UART_RX_GPIO          (GPIO_NUM_29)   // From S3 TX
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
