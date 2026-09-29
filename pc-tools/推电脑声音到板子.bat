@echo off
chcp 65001 >nul
title 把电脑声音推给 ESP32 板子

:: ============================================================
::  板子当"WiFi 音箱"用。
::
::  双击本文件 -> 板子开始放电脑里正在响的声音。
::  然后你照常打开网易云音乐放歌就行,声音会从板子出来。
::
::  Ctrl+C 停止,板子会自动切回 TF 卡播放。
::
::  原理:用 WASAPI 回环直接采【默认输出设备】的样本 ——
::  不用改任何声音路由,不用"立体声混音",也不用装任何东西。
::
::  前提:板子开着机,和电脑在同一个 WiFi。
:: ============================================================

set "WASCAP=%~dp0wascap\wascap.exe"
if not exist "%WASCAP%" (
    echo.
    echo   [X] 找不到 %WASCAP%
    echo       先在 wascap 目录里跑一下 build.bat 编译。
    echo.
    pause
    exit /b 1
)

echo.
echo   正在连接板子并开始采集电脑声音 ...
echo   现在可以去放网易云音乐了。Ctrl+C 停止。
echo.
"%WASCAP%"

echo.
echo   ---- 已停止 ----
pause
