@echo off
chcp 936 >nul
cd /d "%~dp0"
setlocal
set "FILE=app_ota2.bin"
if not "%~1"=="" set "FILE=%~1"
echo.
echo ================================================
echo   Wi-Fi OTA 发送工具   （端口 6677，停等确认）
echo ================================================
echo.
echo   本次发送: %FILE%
echo   （默认发蓝灯版 app_ota2.bin；要发红绿灯版就用:  send_ota.bat app_ota.bin）
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0send_ota.ps1" -File "%FILE%"
echo.
pause