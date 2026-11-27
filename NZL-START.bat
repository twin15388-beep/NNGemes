@echo off
chcp 65001 >nul
title NZL Studio
cd /d "%~dp0"

REM --- self-elevate to administrator (driver load requires it) ---
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [NZL] Requesting administrator rights...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo [NZL] NZL Studio launcher
echo [NZL] 1/3 Preparing kdmapper (skipped if present)...
if not exist "nzl\driver\kdmapper.exe" (
    powershell -ExecutionPolicy Bypass -File "tools\NZL-GetKdmapper.ps1"
)

echo [NZL] 2/3 Loading driver (game must NOT be running yet)...
powershell -ExecutionPolicy Bypass -File "tools\NZL-LoadDriver.ps1"
if %errorlevel% neq 0 (
    echo [NZL] Driver load FAILED. Send a screenshot of this window.
    pause
    exit /b
)

echo.
echo [NZL] Driver loaded. NOW launch ROB RUN via Steam, enter a match,
echo [NZL] then press any key here to start the overlay.
pause >nul

echo [NZL] 3/3 Starting overlay (INSERT = menu, END = exit)...
"build-nzl\Release\nzl_overlay.exe"
pause
