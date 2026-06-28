idf_component_register(
    SRCS "ble_client.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash log
)
