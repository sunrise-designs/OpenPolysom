idf_component_register(
    SRCS "logger.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config edflib esp_driver_sdspi fatfs sdmmc esp_driver_spi log
)
