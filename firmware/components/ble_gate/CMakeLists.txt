idf_component_register(
    SRCS "ble_gate.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_timer esp_system
)
