idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                        bsp_jc1060p470
                        driver
                        esp_lcd
                        lvgl
)
