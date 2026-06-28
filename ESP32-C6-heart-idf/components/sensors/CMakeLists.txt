idf_component_register(
    SRCS "sensors.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_driver_i2c esp_driver_gpio esp_adc log
)
