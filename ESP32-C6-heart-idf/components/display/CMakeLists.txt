idf_component_register(
    SRCS "display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_driver_i2c log
)
