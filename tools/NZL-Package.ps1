<#
NZL Studio - ONE-FILE package builder.
Produces a single NZL-Studio.bat in the project root with driver.sys,
kdmapper.exe and nzl_overlay.exe embedded as base64 inside the .bat.
The recipient gets ONE file: double-click it, it self-elevates,
extracts itself to %LOCALAPPDATA%\NZL-Studio on first use or package update
(settings persist there), skips extraction on later runs, reuses an already
loaded driver or maps it once via kdmapper, waits for the
game, then starts the overlay.
No zip, no folders, no Visual Studio / WDK on the recipient PC.
Usage: powershell -ExecutionPolicy Bypass -File tools\NZL-Package.ps1
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$bin = @{
    "driver.sys"      = "nzl\driver\driver.sys"
    "kdmapper.exe"    = "nzl\driver\kdmapper.exe"
    "nzl_overlay.exe" = "build-nzl\Release\nzl_overlay.exe"
}
foreach ($k in $bin.Keys) {
    if (-not (Test-Path $bin[$k])) {
        Write-Nzl "FAIL: missing $($bin[$k]) - run tools\NZL-BuildAll.ps1 (and NZL-GetKdmapper.ps1) first."
        exit 2
    }
}

$hashParts = @()
foreach ($name in @("driver.sys", "kdmapper.exe", "nzl_overlay.exe")) {
    $filePath = Join-Path $root $bin[$name]
    $fileHash = (Get-FileHash -Path $filePath -Algorithm SHA256).Hash
    $hashParts += ("{0}:{1}" -f $name, $fileHash)
}
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $manifestBytes = [Text.Encoding]::UTF8.GetBytes(($hashParts -join "`n"))
    $manifestHash = $sha.ComputeHash($manifestBytes)
    $packageId = ([BitConverter]::ToString($manifestHash)).Replace("-", "").ToLowerInvariant()
} finally {
    $sha.Dispose()
}

$template = @'
@echo off
chcp 65001 >nul
title NZL Studio
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [NZL] Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
set "NZLDIR=%LOCALAPPDATA%\NZL-Studio"
if not exist "%NZLDIR%" mkdir "%NZLDIR%"
set "NZL_PACKAGE_ID=__PACKAGE_ID__"
set "NZL_INSTALLED_ID="
set "NZL_NEED_EXTRACT=1"
if exist "%NZLDIR%\package.id" for /f "usebackq delims=" %%A in ("%NZLDIR%\package.id") do set "NZL_INSTALLED_ID=%%A"
if "%NZL_INSTALLED_ID%"=="%NZL_PACKAGE_ID%" set "NZL_NEED_EXTRACT=0"
if not exist "%NZLDIR%\driver.sys" set "NZL_NEED_EXTRACT=1"
if not exist "%NZLDIR%\kdmapper.exe" set "NZL_NEED_EXTRACT=1"
if not exist "%NZLDIR%\nzl_overlay.exe" set "NZL_NEED_EXTRACT=1"
if "%NZL_NEED_EXTRACT%"=="0" goto files_cached
echo [NZL] Installing or updating package files...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText('%~f0'); $m=[regex]::Matches($t,'(?ms):::NZL_FILE (\S+)\s+([A-Za-z0-9+/=\s]+?):::NZL_END'); foreach($x in $m){ $b=[Convert]::FromBase64String(($x.Groups[2].Value -replace '\s','')); [IO.File]::WriteAllBytes((Join-Path $env:LOCALAPPDATA ('NZL-Studio\'+$x.Groups[1].Value)), $b) }"
if errorlevel 1 goto extract_failed
> "%NZLDIR%\package.id" echo %NZL_PACKAGE_ID%
goto files_ready
:extract_failed
echo [NZL] Extract FAILED. Check the PowerShell error above.
pause
exit /b 1
:files_cached
echo [NZL] Package files are already current; skipping extraction.
:files_ready
if not exist "%NZLDIR%\nzl_overlay.exe" ( echo [NZL] Overlay EXE missing. & pause & exit /b 1 )
echo [NZL] Files ready in %NZLDIR%
reg query "HKLM\SYSTEM\NaCoreFilter" >nul 2>&1
if errorlevel 1 goto load_driver
reg query "HKLM\SYSTEM\NaCoreFilter" /v Key >nul 2>&1
if errorlevel 1 goto driver_state_bad
reg query "HKLM\SYSTEM\NaCoreFilter" /v Shm >nul 2>&1
if errorlevel 1 goto driver_state_bad
echo [NZL] Driver already loaded in this Windows session - reusing it.
echo [NZL] No reboot is needed to relaunch the overlay.
goto game
:driver_state_bad
echo [NZL] FAIL: driver registry state is incomplete. Refusing to map another copy.
echo [NZL] Reboot Windows before trying to load the driver again.
pause
exit /b 1
:load_driver
tasklist /FI "IMAGENAME eq ROB RUN.exe" | find /I "ROB RUN.exe" >nul
if %errorlevel% equ 0 (
    echo [NZL] FAIL: ROB RUN is already running. Close the game,
    echo [NZL] run this file again, and launch the game only when told.
    pause
    exit /b
)
echo [NZL] Loading driver...
"%NZLDIR%\kdmapper.exe" "%NZLDIR%\driver.sys"
timeout /t 2 /nobreak >nul
reg query "HKLM\SYSTEM\NaCoreFilter" /v Key >nul 2>&1
if %errorlevel% neq 0 (
    echo [NZL] FAIL: driver did not publish its key. Screenshot this window.
    pause
    exit /b
)
echo [NZL] Driver loaded OK.
:game
echo.
echo [NZL] Choose how to start ROB RUN:
echo [1] Launch it now through Steam (AppID 4262460)
echo [2] I will launch it manually; keep waiting
choice /C 12 /N /M "Select 1 or 2: "
if errorlevel 2 goto waitgame
echo [NZL] Opening ROB RUN through Steam...
start "" "steam://rungameid/4262460"
:waitgame
echo [NZL] Waiting for ROB RUN.exe. No key press needed; Ctrl+C cancels.
:waitgame_loop
tasklist /FI "IMAGENAME eq ROB RUN.exe" /NH | find /I "ROB RUN.exe" >nul
if errorlevel 1 (
    timeout /t 2 /nobreak >nul
    goto waitgame_loop
)
echo [NZL] Game process detected. Starting persistent app...
>> "%NZLDIR%\run.log" echo [%date% %time%] overlay starting
"%NZLDIR%\nzl_overlay.exe"
set "NZL_OVERLAY_EXIT=%ERRORLEVEL%"
echo [NZL] Overlay returned exit code %NZL_OVERLAY_EXIT%.
>> "%NZLDIR%\run.log" echo [%date% %time%] overlay exit=%NZL_OVERLAY_EXIT%
echo [NZL] Console will stay open so the exit status is visible.
pause
goto :eof
__PAYLOAD__
'@

$payload = ""
foreach ($name in @("driver.sys", "kdmapper.exe", "nzl_overlay.exe")) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root $bin[$name]))
    $b64 = [Convert]::ToBase64String($bytes)
    $wrapped = [regex]::Replace($b64, ".{76}", "`$0`r`n")
    $payload += ":::NZL_FILE $name`r`n$wrapped`r`n:::NZL_END`r`n"
    Write-Nzl "$name embedded ($($bytes.Length) bytes)"
}
$out = Join-Path $root "NZL-Studio.bat"
$rendered = $template.Replace("__PACKAGE_ID__", $packageId).Replace("__PAYLOAD__", $payload)
[IO.File]::WriteAllText($out, $rendered, [Text.Encoding]::ASCII)
Write-Nzl "Package ID: $packageId"
Write-Nzl "ONE FILE ready: $out ($([math]::Round((Get-Item $out).Length/1MB,2)) MB)"
Write-Nzl "Send ONLY this .bat. Recipient: double-click, admin prompt, choose Steam launch or manual wait; overlay starts automatically when ROB RUN opens."
exit 0
