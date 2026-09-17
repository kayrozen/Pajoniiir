# CMakeLists.txt for bsp_jc1060p470 component

idf_component_register(
    SRCS
        "src/bsp_board.c"
        "src/bsp_display.c"
        "src/bsp_touch.c"
        "src/bsp_audio.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp_lcd
        esp_lcd_touch
        esp_lcd_touch_gt911
        esp_codec_dev
        esp_codec_dev_es8311
        lvgl
        esp_timer
        heap_caps
    PRIV_REQUIRES
        esp_private
)

# Add compile definitions for board-specific settings
target_compile_definitions(${COMPONENT_LIB}
    PUBLIC
        BSP_BOARD_JC1060P470
)
