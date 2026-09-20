# CMakeLists.txt for bsp_jc1060p470 component

idf_component_register(
    SRCS
        "src/bsp_board.c"
        "src/bsp_display.c"
        "src/bsp_touch.c"
        "src/bsp_audio.c"
        "src/bsp_sd.c"
    INCLUDE_DIRS
        "include"
        "include/bsp"
    REQUIRES
        esp_lcd
        esp_lcd_touch
        esp_lcd_touch_gt911
        esp_codec_dev
        lvgl
        esp_lvgl_port
        esp_timer
        heap
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_gpio
        esp_driver_sdmmc
        sdmmc
        fatfs
    PRIV_REQUIRES
        esp_hw_support
)

# Add compile definitions for board-specific settings
target_compile_definitions(${COMPONENT_LIB}
    PUBLIC
        BSP_BOARD_JC1060P470
)
