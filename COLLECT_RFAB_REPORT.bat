@echo off
chcp 65001 >nul
title RFAB Online - report collector

set "PS_SCRIPT=%~dp0Tools\rfab\Collect-RfabReport.ps1"
if not exist "%PS_SCRIPT%" set "PS_SCRIPT=%~dp0Tools\Collect-RfabReport.ps1"
if not exist "%PS_SCRIPT%" set "PS_SCRIPT=%~dp0Collect-RfabReport.ps1"

if not exist "%PS_SCRIPT%" (
    echo [ERROR] Collect-RfabReport.ps1 not found. Extract the whole ZIP archive to a folder first.
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*
echo.
pause
