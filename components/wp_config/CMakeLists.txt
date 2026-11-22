idf_component_register(SRCS "wp_config.c" "wp_config_validate.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash esp_hw_support log)
