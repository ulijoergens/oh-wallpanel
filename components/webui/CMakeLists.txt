idf_component_register(SRCS "webui.c" "webui_crash.c" "webui_ota.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "www/index.html" "www/app.js" "www/style.css"
                       PRIV_REQUIRES esp_http_server esp_hw_support esp_app_format app_update esp_timer log
                                     wp_captive wp_config wp_log wp_oh wp_crash wifi_mgr)
