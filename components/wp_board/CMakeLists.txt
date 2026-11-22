# Both board files are always compiled; each is empty unless its board is selected
# (Kconfig WP_BOARD).
idf_component_register(SRCS "src/board_waveshare43.c"
                            "src/board_sc01plus.c"
                            "src/lvgl_port.c"
                    INCLUDE_DIRS "include"
                    REQUIRES  "esp_timer" "esp_driver_gpio" "esp_driver_i2c" "esp_driver_ledc"
                              "esp_lcd" "esp_lcd_touch" "esp_lcd_touch_gt911"
                              "esp_lcd_touch_ft5x06" "esp_lcd_st7796"
                              "lvgl"
)
