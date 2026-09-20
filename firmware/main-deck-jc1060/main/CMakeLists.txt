idf_component_register(SRCS "main.c" "usb_tu_app.c" "eth_bringup.c" "wifi_console.c" "ota_update.c" "log_screen.c"
                    EMBED_FILES "tone4ch.wav"
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
                        app_update
                        esp_https_ota
                        tinyusb
                        esp_hw_support
                        esp_hosted
)
