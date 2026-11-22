file(GLOB TEST_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../components/*/test/test_*.c")
idf_component_register(SRCS "test_main.c" ${TEST_SRCS}
                       REQUIRES unity wp_captive wp_config wp_log wifi_mgr
                       WHOLE_ARCHIVE)
