idf_component_register(
    SRCS "rt_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config logger esp_wifi esp_http_server esp_netif esp_event nvs_flash freertos log
)
