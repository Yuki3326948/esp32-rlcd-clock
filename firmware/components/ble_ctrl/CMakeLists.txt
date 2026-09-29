idf_component_register(
    SRCS "ble_ctrl.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio
)
