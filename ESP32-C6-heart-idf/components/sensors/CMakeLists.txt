idf_component_register(
    SRCS "sensors.cpp" "MMA8451.cpp" "LDC1612.cpp" "SDP800.cpp" "DS1307.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES board_config esp_driver_i2c esp_driver_gpio esp_adc log
)
