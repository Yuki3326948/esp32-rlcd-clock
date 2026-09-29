idf_component_register(
    SRCS "board_sensors.c"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_gpio
    INCLUDE_DIRS ".")
