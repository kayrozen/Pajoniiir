idf_component_register(
    SRCS "bsp_jc4880_shim.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp_jc1060p470 esp_driver_i2s esp_lcd driver log app_settings
)
