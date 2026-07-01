idf_component_register(
    SRCS "display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc log
)
