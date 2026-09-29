idf_component_register(
    SRCS "audio_player.c"
    REQUIRES esp_driver_i2s esp_driver_i2c board_sensors
    PRIV_REQUIRES espressif__esp_codec_dev
    INCLUDE_DIRS ".")
