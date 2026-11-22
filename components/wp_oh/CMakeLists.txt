idf_component_register(SRCS "wp_oh.c" "wp_model.c"
                       INCLUDE_DIRS "include"
                       REQUIRES wp_config
                       PRIV_REQUIRES esp_http_client esp-tls esp_timer log wifi_mgr)
