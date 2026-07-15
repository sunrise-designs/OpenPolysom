idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        board_config
        time_sync
        sensors
        logger
        display
        battery_gauge
        freertos
        esp_driver_gpio
        log
)
