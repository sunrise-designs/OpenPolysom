idf_component_register(
    SRCS "DS3231.cpp" "sensors.cpp" "MMA8451.cpp" "LDC1612.cpp" "SDP800.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES board_config esp_driver_i2c esp_driver_gpio esp_adc log
)
