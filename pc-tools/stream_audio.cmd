@echo off
chcp 65001 >nul
setlocal
title 推音乐到 ESP32 音箱
cd /d "%~dp0"

rem ---------------- 检查 Python ----------------
set "PY="
where py >nul 2>nul && set "PY=py"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo.
    echo   [X] 电脑上没找到 Python,请先装一个 Python 3。
    echo.
    pause
    exit /b 1
)

rem ---------------- 检查 ffmpeg ----------------
where ffmpeg >nul 2>nul
if errorlevel 1 (
    echo.
    echo   [X] 没找到 ffmpeg。装法:打开命令行敲
    echo         winget install Gyan.FFmpeg
    echo       装完关掉这个窗口重新双击一次。
    echo.
    pause
    exit /b 1
)

rem ---------------- 拿要放的文件 ----------------
set "FILE=%~1"
if not defined FILE (
    echo.
    echo   把要放的音频文件【拖进这个窗口】,然后按回车。
    echo   MP3 / FLAC / M4A / WAV / OGG,连视频文件也能放。
    echo.
    set /p "FILE=   文件: "
)
set "FILE=%FILE:"=%"

if not exist "%FILE%" (
    echo.
    echo   [X] 找不到这个文件: %FILE%
    echo.
    pause
    exit /b 1
)

echo.
%PY% "%~dp0stream_audio.py" "%FILE%" --loop

echo.
echo   已停止,板子已回到 TF 卡播放。
pause
