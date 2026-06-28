idf_component_register(
    SRCS "wifi_ntp.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_wifi esp_event esp_netif lwip nvs_flash log
)
