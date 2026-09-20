#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"

#include "esp_lcd_jd9165.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "lcd.h"
#include "pingcfg.h"

#define TAG "lcd"

#define HW_LDO_MIPI_CHAN                        (3)
#define HW_LDO_MIPI_VOLTAGE_MV                  (2500)
#define HW_LCD_BIT_PER_PIXEL                    (16)
#define HW_MIPI_DPI_PX_FORMAT                   (LCD_COLOR_PIXEL_FORMAT_RGB565)

#define HW_I2C_NUM                          (I2C_NUM_0)
#define HW_I2C_CLK_SPEED_HZ                 (400000)

#define TOUCH_ROTATION_TYPE             TOUCH_ROTATION_MIPI_DSI

static esp_lcd_dsi_bus_handle_t s_mipi_dsi_bus;
static esp_lcd_panel_io_handle_t s_mipi_dbi_io;
static esp_lcd_panel_handle_t s_panel_handle;
static esp_ldo_channel_handle_t s_ldo_mipi_phy;

static i2c_master_bus_handle_t s_touch_i2c_bus = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static esp_lcd_panel_io_handle_t s_touch_io_handle = NULL;

#if JC1060P470
static const jd9165_lcd_init_cmd_t lcd_cmd[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49,0x61,0x02,0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x00}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x00}, 1, 0},
    {0x25, (uint8_t[]){0x19}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x04}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x04}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x00}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x0A, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x0A,0x1A,0x0B,0x0D,0x0D,0x11,0x10,0x06,0x08,0x1F,0x1D}, 11, 0},
    {0x0C, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x16,0x1B,0x0B,0x0D,0x0D,0x11,0x10,0x07,0x09,0x1E,0x1C}, 11, 0},
    {0x0E, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x16,0x1B,0x0D,0x0B,0x0D,0x11,0x10,0x1C,0x1E,0x09,0x07}, 11, 0},
    {0x10, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x0A,0x1A,0x0D,0x0B,0x0D,0x11,0x10,0x1D,0x1F,0x08,0x06}, 11, 0},
    {0x12, (uint8_t[]){0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x14, (uint8_t[]){0x00,0x00,0x11,0x11}, 4, 0},
    {0x18, (uint8_t[]){0x99}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x36,0x2C,0x2E,0x3C,0x38,0x35,0x35,0x32,0x2E,0x1D,0x2B,0x21,0x16,0x29}, 14, 0},
    {0x13, (uint8_t[]){0x36,0x2C,0x2E,0x3C,0x38,0x35,0x35,0x32,0x2E,0x1D,0x2B,0x21,0x16,0x29}, 14, 0},
    
    // {0x30, (uint8_t[]){0x08}, 1, 0},
    // {0x05, (uint8_t[]){0x01}, 1, 0},
    // {0x0C, (uint8_t[]){0x1A}, 1, 0},
    // {0x0D, (uint8_t[]){0x0E}, 1, 0},

    // {0x30, (uint8_t[]){0x07}, 1, 0},
    // {0x01, (uint8_t[]){0x04}, 1, 0},

    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x12, (uint8_t[]){0x3E}, 1, 0},
    {0x13, (uint8_t[]){0x78}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x0C}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x0C}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},

    {0X3A, (uint8_t[]){0x55}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},

};
#elif JC8012P4A1
static const jd9165_lcd_init_cmd_t lcd_cmd[] = {
 //  {cmd, { data }, data_size, delay_ms}
    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0xE1, (uint8_t[]){0x93}, 1, 0},
    {0xE2, (uint8_t[]){0x65}, 1, 0},
    {0xE3, (uint8_t[]){0xF8}, 1, 0},
    {0x80, (uint8_t[]){0x01}, 1, 0},

    {0xE0, (uint8_t[]){0x01}, 1, 0},
    {0x00, (uint8_t[]){0x00}, 1, 0},
    {0x01, (uint8_t[]){0x39}, 1, 0},
    {0x03, (uint8_t[]){0x10}, 1, 0},
    {0x04, (uint8_t[]){0x41}, 1, 0},

    {0x0C, (uint8_t[]){0x74}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0xD7}, 1, 0},
    {0x19, (uint8_t[]){0x00}, 1, 0},
    {0x1A, (uint8_t[]){0x00}, 1, 0},

    {0x1B, (uint8_t[]){0xD7}, 1, 0},
    {0x1C, (uint8_t[]){0x00}, 1, 0},
    {0x24, (uint8_t[]){0xFE}, 1, 0},
    {0x35, (uint8_t[]){0x26}, 1, 0},
    {0x37, (uint8_t[]){0x69}, 1, 0},

    {0x38, (uint8_t[]){0x05}, 1, 0},
    {0x39, (uint8_t[]){0x06}, 1, 0},
    {0x3A, (uint8_t[]){0x08}, 1, 0},
    {0x3C, (uint8_t[]){0x78}, 1, 0},
    {0x3D, (uint8_t[]){0xFF}, 1, 0},

    {0x3E, (uint8_t[]){0xFF}, 1, 0},
    {0x3F, (uint8_t[]){0xFF}, 1, 0},
    {0x40, (uint8_t[]){0x06}, 1, 0},
    {0x41, (uint8_t[]){0xA0}, 1, 0},
    {0x43, (uint8_t[]){0x14}, 1, 0},

    {0x44, (uint8_t[]){0x0B}, 1, 0},
    {0x45, (uint8_t[]){0x30}, 1, 0},
    //{0x4A, (uint8_t[]){0x35}, 1, 0},//bist
    {0x4B, (uint8_t[]){0x04}, 1, 0},
    {0x55, (uint8_t[]){0x02}, 1, 0},
    {0x57, (uint8_t[]){0x89}, 1, 0},

    {0x59, (uint8_t[]){0x0A}, 1, 0},
    {0x5A, (uint8_t[]){0x28}, 1, 0},

    {0x5B, (uint8_t[]){0x15}, 1, 0},
    {0x5D, (uint8_t[]){0x50}, 1, 0},
    {0x5E, (uint8_t[]){0x37}, 1, 0},
    {0x5F, (uint8_t[]){0x29}, 1, 0},
    {0x60, (uint8_t[]){0x1E}, 1, 0},

    {0x61, (uint8_t[]){0x1D}, 1, 0},
    {0x62, (uint8_t[]){0x12}, 1, 0},
    {0x63, (uint8_t[]){0x1A}, 1, 0},
    {0x64, (uint8_t[]){0x08}, 1, 0},
    {0x65, (uint8_t[]){0x25}, 1, 0},

    {0x66, (uint8_t[]){0x26}, 1, 0},
    {0x67, (uint8_t[]){0x28}, 1, 0},
    {0x68, (uint8_t[]){0x49}, 1, 0},
    {0x69, (uint8_t[]){0x3A}, 1, 0},
    {0x6A, (uint8_t[]){0x43}, 1, 0},

    {0x6B, (uint8_t[]){0x3A}, 1, 0},
    {0x6C, (uint8_t[]){0x3B}, 1, 0},
    {0x6D, (uint8_t[]){0x32}, 1, 0},
    {0x6E, (uint8_t[]){0x1F}, 1, 0},
    {0x6F, (uint8_t[]){0x0E}, 1, 0},

    {0x70, (uint8_t[]){0x50}, 1, 0},
    {0x71, (uint8_t[]){0x37}, 1, 0},
    {0x72, (uint8_t[]){0x29}, 1, 0},
    {0x73, (uint8_t[]){0x1E}, 1, 0},
    {0x74, (uint8_t[]){0x1D}, 1, 0},

    {0x75, (uint8_t[]){0x12}, 1, 0},
    {0x76, (uint8_t[]){0x1A}, 1, 0},
    {0x77, (uint8_t[]){0x08}, 1, 0},
    {0x78, (uint8_t[]){0x25}, 1, 0},
    {0x79, (uint8_t[]){0x26}, 1, 0},

    {0x7A, (uint8_t[]){0x28}, 1, 0},
    {0x7B, (uint8_t[]){0x49}, 1, 0},
    {0x7C, (uint8_t[]){0x3A}, 1, 0},
    {0x7D, (uint8_t[]){0x43}, 1, 0},
    {0x7E, (uint8_t[]){0x3A}, 1, 0},

    {0x7F, (uint8_t[]){0x3B}, 1, 0},
    {0x80, (uint8_t[]){0x32}, 1, 0},
    {0x81, (uint8_t[]){0x1F}, 1, 0},
    {0x82, (uint8_t[]){0x0E}, 1, 0},
    {0xE0,(uint8_t []){0x02},1,0},

    {0x00,(uint8_t []){0x1F},1,0},
    {0x01,(uint8_t []){0x1F},1,0},
    {0x02,(uint8_t []){0x52},1,0},
    {0x03,(uint8_t []){0x51},1,0},
    {0x04,(uint8_t []){0x50},1,0},

    {0x05,(uint8_t []){0x4B},1,0},
    {0x06,(uint8_t []){0x4A},1,0},
    {0x07,(uint8_t []){0x49},1,0},
    {0x08,(uint8_t []){0x48},1,0},
    {0x09,(uint8_t []){0x47},1,0},

    {0x0A,(uint8_t []){0x46},1,0},
    {0x0B,(uint8_t []){0x45},1,0},
    {0x0C,(uint8_t []){0x44},1,0},
    {0x0D,(uint8_t []){0x40},1,0},
    {0x0E,(uint8_t []){0x41},1,0},

    {0x0F,(uint8_t []){0x1F},1,0},
    {0x10,(uint8_t []){0x1F},1,0},
    {0x11,(uint8_t []){0x1F},1,0},
    {0x12,(uint8_t []){0x1F},1,0},
    {0x13,(uint8_t []){0x1F},1,0},

    {0x14,(uint8_t []){0x1F},1,0},
    {0x15,(uint8_t []){0x1F},1,0},
    {0x16,(uint8_t []){0x1F},1,0},
    {0x17,(uint8_t []){0x1F},1,0},
    {0x18,(uint8_t []){0x52},1,0},

    {0x19,(uint8_t []){0x51},1,0},
    {0x1A,(uint8_t []){0x50},1,0},
    {0x1B,(uint8_t []){0x4B},1,0},
    {0x1C,(uint8_t []){0x4A},1,0},
    {0x1D,(uint8_t []){0x49},1,0},

    {0x1E,(uint8_t []){0x48},1,0},
    {0x1F,(uint8_t []){0x47},1,0},
    {0x20,(uint8_t []){0x46},1,0},
    {0x21,(uint8_t []){0x45},1,0},
    {0x22,(uint8_t []){0x44},1,0},

    {0x23,(uint8_t []){0x40},1,0},
    {0x24,(uint8_t []){0x41},1,0},
    {0x25,(uint8_t []){0x1F},1,0},
    {0x26,(uint8_t []){0x1F},1,0},
    {0x27,(uint8_t []){0x1F},1,0},

    {0x28,(uint8_t []){0x1F},1,0},
    {0x29,(uint8_t []){0x1F},1,0},
    {0x2A,(uint8_t []){0x1F},1,0},
    {0x2B,(uint8_t []){0x1F},1,0},
    {0x2C,(uint8_t []){0x1F},1,0},

    {0x2D,(uint8_t []){0x1F},1,0},
    {0x2E,(uint8_t []){0x52},1,0},
    {0x2F,(uint8_t []){0x40},1,0},
    {0x30,(uint8_t []){0x41},1,0},
    {0x31,(uint8_t []){0x48},1,0},

    {0x32,(uint8_t []){0x49},1,0},
    {0x33,(uint8_t []){0x4A},1,0},
    {0x34,(uint8_t []){0x4B},1,0},
    {0x35,(uint8_t []){0x44},1,0},
    {0x36,(uint8_t []){0x45},1,0},

    {0x37,(uint8_t []){0x46},1,0},
    {0x38,(uint8_t []){0x47},1,0},
    {0x39,(uint8_t []){0x51},1,0},
    {0x3A,(uint8_t []){0x50},1,0},
    {0x3B,(uint8_t []){0x1F},1,0},

    {0x3C,(uint8_t []){0x1F},1,0},
    {0x3D,(uint8_t []){0x1F},1,0},
    {0x3E,(uint8_t []){0x1F},1,0},
    {0x3F,(uint8_t []){0x1F},1,0},
    {0x40,(uint8_t []){0x1F},1,0},

    {0x41,(uint8_t []){0x1F},1,0},
    {0x42,(uint8_t []){0x1F},1,0},
    {0x43,(uint8_t []){0x1F},1,0},
    {0x44,(uint8_t []){0x52},1,0},
    {0x45,(uint8_t []){0x40},1,0},

    {0x46,(uint8_t []){0x41},1,0},
    {0x47,(uint8_t []){0x48},1,0},
    {0x48,(uint8_t []){0x49},1,0},
    {0x49,(uint8_t []){0x4A},1,0},
    {0x4A,(uint8_t []){0x4B},1,0},

    {0x4B,(uint8_t []){0x44},1,0},
    {0x4C,(uint8_t []){0x45},1,0},
    {0x4D,(uint8_t []){0x46},1,0},
    {0x4E,(uint8_t []){0x47},1,0},
    {0x4F,(uint8_t []){0x51},1,0},

    {0x50,(uint8_t []){0x50},1,0},
    {0x51,(uint8_t []){0x1F},1,0},
    {0x52,(uint8_t []){0x1F},1,0},
    {0x53,(uint8_t []){0x1F},1,0},
    {0x54,(uint8_t []){0x1F},1,0},

    {0x55,(uint8_t []){0x1F},1,0},
    {0x56,(uint8_t []){0x1F},1,0},
    {0x57,(uint8_t []){0x1F},1,0},
    {0x58,(uint8_t []){0x40},1,0},
    {0x59,(uint8_t []){0x00},1,0},

    {0x5A,(uint8_t []){0x00},1,0},
    {0x5B,(uint8_t []){0x10},1,0},
    {0x5C,(uint8_t []){0x05},1,0},
    {0x5D,(uint8_t []){0x50},1,0},
    {0x5E,(uint8_t []){0x01},1,0},

    {0x5F,(uint8_t []){0x02},1,0},
    {0x60,(uint8_t []){0x50},1,0},
    {0x61,(uint8_t []){0x06},1,0},
    {0x62,(uint8_t []){0x04},1,0},
    {0x63,(uint8_t []){0x03},1,0},

    {0x64,(uint8_t []){0x64},1,0},
    {0x65,(uint8_t []){0x65},1,0},
    {0x66,(uint8_t []){0x0B},1,0},
    {0x67,(uint8_t []){0x73},1,0},
    {0x68,(uint8_t []){0x07},1,0},

    {0x69,(uint8_t []){0x06},1,0},
    {0x6A,(uint8_t []){0x64},1,0},
    {0x6B,(uint8_t []){0x08},1,0},
    {0x6C,(uint8_t []){0x00},1,0},
    {0x6D,(uint8_t []){0x32},1,0},

    {0x6E,(uint8_t []){0x08},1,0},
    {0xE0,(uint8_t []){0x04},1,0},
    {0x2C,(uint8_t []){0x6B},1,0},
    {0x35,(uint8_t []){0x08},1,0},
    {0x37,(uint8_t []){0x00},1,0},

    {0xE0,(uint8_t []){0x00},1,0},
    {0x11,(uint8_t []){0x00},1,0},
    {0x29, (uint8_t[]){0x00}, 1, 5},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x35, (uint8_t[]){0x00}, 1, 0},
};
#elif JC4880P443
static const jd9165_lcd_init_cmd_t lcd_cmd[] = {
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x13},5,0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x10},5,0},
    {0xC0, (uint8_t []){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},

    {0xB0, (uint8_t []){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t []){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

    {0xB0, (uint8_t []){0x5D}, 1, 0},
    {0xB1, (uint8_t []){0x58}, 1, 0},
    {0xB2, (uint8_t []){0x87}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4E}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0},
    {0xB9, (uint8_t []){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t []){0x03}, 1,0},
    {0xBC, (uint8_t []){0x00}, 1,0},
    
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},

    {0xE0, (uint8_t []){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x04, 0xA0, 0x00, 0xA0, 0x05,0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t []){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

    {0xEB, (uint8_t []){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t []){0x08, 0x01}, 2, 0},

    {0xED, (uint8_t []){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    // {0x3A, (uint8_t []){0x66}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 20},

};
#endif

static esp_err_t hw_touch_i2c_init(void)
{
    if (s_touch_i2c_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .i2c_port = HW_I2C_NUM,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_conf, &s_touch_i2c_bus));

    return ESP_OK;
}
static void lcd_ldo_power_on(void)
{
    if (s_ldo_mipi_phy) {
        return;
    }

    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = HW_LDO_MIPI_CHAN,
        .voltage_mv = HW_LDO_MIPI_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &s_ldo_mipi_phy));
}
void lcd_init(esp_lcd_panel_handle_t *panel_handle, esp_lcd_panel_io_handle_t *io_handle, esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode, esp_lv_adapter_rotation_t rotation)
{
    lcd_ldo_power_on();

    ESP_LOGD(TAG, "Install LCD driver");

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,                                     
        .num_data_lanes = 2,                             
        .phy_clk_src = 0,    
#if JC1060P470                            
        .lane_bit_rate_mbps = 750,  
#elif JC4880P443
        .lane_bit_rate_mbps = 500,  
#elif JC8012P4A1
        .lane_bit_rate_mbps = 1000,  
#endif
    };

    
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &s_mipi_dsi_bus));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_dbi_io_config_t dbi_config = {                      
        .virtual_channel = 0,         
        .lcd_cmd_bits = 8,            
        .lcd_param_bits = 8,          
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_mipi_dsi_bus, &dbi_config, &s_mipi_dbi_io));

    ESP_LOGI(TAG, "Install LCD driver of ek79007");
    esp_lcd_dpi_panel_config_t dpi_config = {
#if JC1060P470                            
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,    
        .dpi_clock_freq_mhz = 52,                       
        .virtual_channel = 0,                           
        .pixel_format = HW_MIPI_DPI_PX_FORMAT,                      
        .num_fbs = 1,                                   
        .video_timing = {                               
            .h_size = 1024,                             
            .v_size = 600,                              
            .hsync_back_porch = 136,                    
            .hsync_pulse_width = 24,                    
            .hsync_front_porch = 160,                   
            .vsync_back_porch = 21,                     
            .vsync_pulse_width = 2,                     
            .vsync_front_porch = 12,                    
        },                                              
        .flags.use_dma2d = true,                          
#elif JC4880P443
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,    
        .dpi_clock_freq_mhz = 28,                       
        .virtual_channel = 0,                           
        .pixel_format = HW_MIPI_DPI_PX_FORMAT,                      
        .num_fbs = 1,                                   
        .video_timing = {                               
            .h_size = 480,                             
            .v_size = 800,                              
            .hsync_back_porch = 42,                    
            .hsync_pulse_width = 12,                    
            .hsync_front_porch = 42,                   
            .vsync_back_porch = 8,                     
            .vsync_pulse_width = 2,                     
            .vsync_front_porch = 166,                    
        },                                              
        .flags.use_dma2d = true,
#elif JC8012P4A1
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,    
        .dpi_clock_freq_mhz = 60,                       
        .virtual_channel = 0,                           
        .pixel_format = HW_MIPI_DPI_PX_FORMAT,                      
        .num_fbs = 1,                                   
        .video_timing = {                               
            .h_size = 800,                             
            .v_size = 1280,                              
            .hsync_back_porch = 20,                    
            .hsync_pulse_width = 20,                    
            .hsync_front_porch = 40,                   
            .vsync_back_porch = 4,                     
            .vsync_pulse_width = 8,                     
            .vsync_front_porch = 20,                    
        },                                              
        .flags.use_dma2d = true, 
#endif
    };
    
    dpi_config.num_fbs = esp_lv_adapter_get_required_frame_buffer_count(tear_avoid_mode, rotation);
    jd9165_vendor_config_t vendor_config = {
        .init_cmds = lcd_cmd,
        .init_cmds_size = sizeof(lcd_cmd) / sizeof(jd9165_lcd_init_cmd_t),
        .mipi_config = {
            .dsi_bus = s_mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = HW_LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9165(s_mipi_dbi_io, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    *panel_handle = s_panel_handle;
    if (io_handle) {
        *io_handle = s_mipi_dbi_io;
    }

}
void touch_init(esp_lcd_touch_handle_t *ret_touch, esp_lv_adapter_rotation_t rotation)
{
    hw_touch_i2c_init();

    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    // touch_get_rotation_flags(TOUCH_ROTATION_TYPE, rotation, &swap_xy, &mirror_x, &mirror_y);

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = HW_LCD_H_RES,
        .y_max = HW_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_err_t ret = ESP_OK;

#if JC1060P470 | JC4880P443
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = HW_I2C_CLK_SPEED_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_touch_i2c_bus, &tp_io_config, &tp_io_handle));
    ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
#elif JC8012P4A1
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG();
    tp_io_config.scl_speed_hz = HW_I2C_CLK_SPEED_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_touch_i2c_bus, &tp_io_config, &tp_io_handle));
    ret = esp_lcd_touch_new_i2c_gsl3680(tp_io_handle, &tp_cfg, ret_touch);
#endif

    if (ret != ESP_OK) {
        if (tp_io_handle) {
            esp_lcd_panel_io_del(tp_io_handle);
        }
        return;
    }

    s_touch_handle = *ret_touch;
    s_touch_io_handle = tp_io_handle;
}

void lcd_backlight_init(void)
{
     // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = LCD_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    ESP_ERROR_CHECK(ledc_timer_config(&LCD_backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&LCD_backlight_channel));
}
void lcd_backlight_on(void)
{
    return lcd_backlight_set_level(100);
}
void lcd_backlight_off(void)
{
    return lcd_backlight_set_level(0);
}

//level 0-100
void lcd_backlight_set_level(int brightness_percent)
{
     if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023 * brightness_percent) / 100; // LEDC resolution set to 10bits, thus: 100% = 1023
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    return;
}