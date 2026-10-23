@echo off
chcp 65001 >nul
title RFAB Online Dedicated Server (UDP 10578)
cd /d "%~dp0"
if exist "%~dp0Run-Server.ps1" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Server.ps1"
) else (
    set TILTED_ACCEPT_EULA=true
    "%~dp0SkyrimTogetherServer.exe"
)
pause
