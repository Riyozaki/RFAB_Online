@echo off
chcp 65001 >nul
title RFAB Online - Ustanovka Modpaka (Client MO2)
if exist "%~dp0Tools\Setup-RfabOnline.ps1" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Tools\Setup-RfabOnline.ps1" -SkipFriendArchive %*
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup-RfabOnline.ps1" -SkipFriendArchive %*
)
if errorlevel 1 (
    echo.
    echo [ERROR] Ustanovka zavershilas s oshibkoy. Smotrite soobshchenie vyshe.
    pause
    exit /b 1
)
echo.
pause
