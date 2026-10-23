@echo off
chcp 65001 >nul
title RFAB Online - Build, Package & Auto-Deploy (SE 1.5.97)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Tools\rfab\Deploy-RfabOnline.ps1" %*
if errorlevel 1 (
    echo.
    echo [ERROR] Sborka ili ustanovka zavershilas s oshibkoy. Smotrite soobshchenie vyshe.
    pause
    exit /b 1
)
echo.
pause
