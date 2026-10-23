@echo off
chcp 65001 >nul
title RFAB Online - Auto Setup (SE 1.5.97 + MO2)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Tools\rfab\Setup-RfabOnline.ps1" %*
if errorlevel 1 (
    echo.
    echo [ERROR] Setup finished with an error. Check messages above.
    pause
    exit /b 1
)
echo.
pause
