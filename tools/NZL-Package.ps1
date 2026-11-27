<#
NZL Studio - ONE-FILE package builder.
Produces a single NZL-Studio.bat in the project root with driver.sys,
kdmapper.exe and nzl_overlay.exe embedded as base64 inside the .bat.
The recipient gets ONE file: double-click it, it self-elevates,
extracts itself to %LOCALAPPDATA%\NZL-Studio (settings persist there),
loads the driver via kdmapper, waits for the game, starts the overlay.
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
echo [NZL] Extracting...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText('%~f0'); $m=[regex]::Matches($t,'(?ms):::NZL_FILE (\S+)\s+([A-Za-z0-9+/=\s]+?):::NZL_END'); foreach($x in $m){ $b=[Convert]::FromBase64String(($x.Groups[2].Value -replace '\s','')); [IO.File]::WriteAllBytes((Join-Path $env:LOCALAPPDATA ('NZL-Studio\'+$x.Groups[1].Value)), $b) }"
if not exist "%NZLDIR%\nzl_overlay.exe" ( echo [NZL] Extract FAILED. & pause & exit /b )
echo [NZL] Files ready in %NZLDIR%
reg query "HKLM\SYSTEM\NaCoreFilter" /v Key >nul 2>&1
if %errorlevel% equ 0 (
    echo [NZL] Driver already loaded in this Windows session - skipping.
    goto game
)
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
echo [NZL] NOW launch ROB RUN via Steam and enter a match,
echo [NZL] then press any key here to start the overlay.
pause >nul
echo [NZL] Overlay starting (INSERT = menu, END = exit)...
"%NZLDIR%\nzl_overlay.exe"
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
[IO.File]::WriteAllText($out, $template.Replace("__PAYLOAD__", $payload), [Text.Encoding]::ASCII)
Write-Nzl "ONE FILE ready: $out ($([math]::Round((Get-Item $out).Length/1MB,2)) MB)"
Write-Nzl "Send ONLY this .bat. Recipient: double-click, admin prompt, launch game when told, INSERT = menu."
exit 0
