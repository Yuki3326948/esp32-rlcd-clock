idf_component_register(
    SRCS "sysmon.c"
    INCLUDE_DIRS "."
    REQUIRES lwip esp_timer sdcard
)
