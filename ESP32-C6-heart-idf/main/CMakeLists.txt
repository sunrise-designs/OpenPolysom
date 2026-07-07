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
        file_server
        nvs_flash
        freertos
        esp_driver_gpio
        log
)
