idf_component_register(SRCS "wp_oh.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client esp-tls log wp_config wifi_mgr)
