idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        board_config
        wifi_ntp
        sensors
        logger
        display
        battery_gauge
        nvs_flash
        freertos
        esp_driver_gpio
        log
)
