# All board files are always compiled; each is empty unless its board is selected
# (Kconfig WP_BOARD). The panel/touch drivers are picked per chip (see idf_component.yml).
set(reqs "esp_timer" "esp_driver_gpio" "esp_driver_i2c" "esp_driver_ledc" "esp_lcd" "esp_lcd_touch" "lvgl")
if(IDF_TARGET STREQUAL "esp32c3")
    list(APPEND reqs "esp_driver_spi" "esp_lcd_gc9a01" "esp_lcd_touch_cst816s")
else()
    list(APPEND reqs "esp_lcd_touch_gt911" "esp_lcd_touch_ft5x06" "esp_lcd_st7796")
endif()
idf_component_register(SRCS "src/board_waveshare43.c"
                            "src/board_sc01plus.c"
                            "src/board_2424s012.c"
                            "src/lvgl_port.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ${reqs}
)
