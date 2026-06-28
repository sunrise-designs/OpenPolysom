idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        board_config
        wifi_ntp
        ble_client
        sensors
        logger
        display
        nvs_flash
        esp_driver_uart
        freertos
        log
)
