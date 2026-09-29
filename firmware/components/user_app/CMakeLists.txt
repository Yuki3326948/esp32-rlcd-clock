idf_component_register(
    SRCS "user_app.cpp"
    PRIV_REQUIRES main esp_driver_gpio port_bsp
                   esp_wifi esp_netif esp_event nvs_flash lwip
                   ui_draw board_sensors audio_player spectrum sdcard wav_player
                   ble_ctrl net_audio battery sysmon
    INCLUDE_DIRS "./")

# ============================================================
#  编"给别人用"的固件时打开这个开关:默认 WiFi 凭据留空,
#  让对方用 TF 卡根目录的 wifi.txt 配网(见 user_app.cpp 里的 WifiLoadFromSd)。
#
#       idf.py -DWIFI_CREDENTIALS_EMPTY=ON build
#
#  用哨兵宏而不是直接传 -DWIFI_SSID="":CMake 传空字符串时
#  引号很容易被吃掉,编出来会变成语法错误或者意外赋了别的值。
# ============================================================
option(WIFI_CREDENTIALS_EMPTY
       "Build without built-in WiFi credentials (configure via /sdcard/wifi.txt)" OFF)
if(WIFI_CREDENTIALS_EMPTY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_CREDENTIALS_EMPTY=1)
endif()

# ============================================================
#  固定 IP 不再用构建开关控制,而是看 wifi_secrets.h 里
#  有没有定义 STATIC_IP_0..3 / STATIC_GW_0..3。
#
#  为什么改成这样:
#    公开仓库的默认值必须能直接用在任何网络上,所以默认必须走 DHCP;
#    而"自己想用固定地址"是【私事】—— 放在不进版本库的 wifi_secrets.h 里
#    正合适,不会把家里网段漏出去,也不用每次编译都记得加命令行参数。
# ============================================================
