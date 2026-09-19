idf_component_register(SRCS "main.c" "usb_host_bringup.c" "eth_bringup.c" "wifi_console.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                        bsp_jc1060p470
                        driver
                        esp_lcd
                        lvgl
                        nvs_flash
                        esp_wifi
                        esp_eth
                        esp_event
)
