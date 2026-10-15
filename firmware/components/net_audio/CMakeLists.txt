idf_component_register(
    SRCS "net_audio.c"
    INCLUDE_DIRS "."
    REQUIRES lwip audio_player spectrum esp_wifi esp_timer esp_driver_usb_serial_jtag
)
