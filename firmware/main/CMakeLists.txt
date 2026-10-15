idf_component_register(
    SRCS "usb_test.c" "main.cpp"
    REQUIRES
        esp_timer
        port_bsp
        user_app
        u8g2_st7305
        esp_driver_usb_serial_jtag
    INCLUDE_DIRS "./")
