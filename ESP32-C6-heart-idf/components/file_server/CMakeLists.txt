idf_component_register(
    SRCS "file_server.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config display esp_wifi esp_event esp_netif esp_http_server log
)
