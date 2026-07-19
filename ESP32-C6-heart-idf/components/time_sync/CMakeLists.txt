idf_component_register(
    SRCS "time_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_driver_usb_serial_jtag esp_driver_gpio freertos esp_timer log
)
