idf_component_register(
    SRCS "display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config sensors logger ble_client esp_lcd esp_driver_spi esp_driver_gpio log
)
