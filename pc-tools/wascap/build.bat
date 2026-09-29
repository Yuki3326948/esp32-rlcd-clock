@echo off
chcp 65001 >nul
title 编译 wascap
:: ============================================================
::  编译 wascap.exe —— WASAPI 回环采集 + 推给板子。
::  零依赖:直接用系统自带的 csc.exe,不需要装任何东西。
:: ============================================================
setlocal
set "CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"

if not exist "%CSC%" (
    echo.
    echo   [X] 找不到 csc.exe:%CSC%
    echo.
    pause
    exit /b 1
)

echo 编译中 ...
"%CSC%" /nologo /target:exe /platform:x64 /optimize+ /unsafe ^
        /out:"%~dp0wascap.exe" "%~dp0wascap.cs"

if errorlevel 1 (
    echo.
    echo   编译失败!上面有具体报错。
    echo.
    pause
    exit /b 1
)

echo.
echo   编译成功:%~dp0wascap.exe
echo.
echo   用法:
echo     wascap.exe --list                 列出所有输出设备(带混音格式)
echo     wascap.exe                        推【默认输出设备】的声音到板子
echo     wascap.exe --device USB           按名字子串指定输出设备
echo     wascap.exe --host 192.168.1.182   手动指定板子 IP
echo     wascap.exe --gain 0.5             软件衰减(0~4)
echo.
echo   平时直接双击上一级目录的「推电脑声音到板子.bat」更省事。
echo.
pause
