idf_component_register(
    SRCS "time_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart freertos esp_timer log
)
