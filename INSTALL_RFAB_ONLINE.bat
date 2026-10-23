@echo off
chcp 65001 >nul
title RFAB Online - Clean Standalone Installer (Skyrim SE 1.5.97.0.8)

set "PS_SCRIPT=%~dp0Tools\rfab\Setup-RfabOnline.ps1"
if not exist "%PS_SCRIPT%" set "PS_SCRIPT=%~dp0Tools\Setup-RfabOnline.ps1"

if not exist "%PS_SCRIPT%" (
    echo [ERROR] Setup-RfabOnline.ps1 not found! Please extract the entire ZIP archive to a folder first.
    echo %* | find /I "NonInteractive" >nul || pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*
if errorlevel 1 (
    echo.
    echo [ERROR] Installation did not complete. Read the message above.
    echo %* | find /I "NonInteractive" >nul || pause
    exit /b 1
)

echo.
echo %* | find /I "NonInteractive" >nul || pause
