idf_component_register(SRCS "main.c" "console.c" "time_sync.c" "ui.c" "ui_tiles.c" "ui_dimmer.c" "lv_mem_psram.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES wp_board wp_config wp_log wp_crash wp_oh webui wifi_mgr
                                     console esp_driver_usb_serial_jtag app_update esp_netif esp_wifi esp_event nvs_flash esp_timer)

# lv_mem_psram.c is only referenced from LVGL, which links after main: force it in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
