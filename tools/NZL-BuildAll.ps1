<#
NZL Studio - build everything (Windows, run from NZL project root).
1) Driver: nzl\driver\build_driver.bat (MSVC+WDK) -> driver.sys
2) NZL executables: cmake (if present) or MSVC directly -> build-nzl\nzl_probe.exe
Usage:
  powershell -ExecutionPolicy Bypass -File tools\NZL-BuildAll.ps1
#>
$ErrorActionPreference = "Continue"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

Write-Nzl "1/2 driver (MSVC+WDK)..."
Push-Location (Join-Path $root "nzl\driver")
& .\build_driver.bat
$drv = Join-Path $root "nzl\driver\driver.sys"
Pop-Location
if (Test-Path $drv) { Write-Nzl "driver.sys OK: $drv" } else { Write-Nzl "driver.sys NOT built - send me the red error text above." }

Write-Nzl "2/2 NZL executables..."
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    & cmake -S nzl -B build-nzl -A x64
    & cmake --build build-nzl --config Release
    if (Test-Path "build-nzl\Release\nzl_probe.exe") { Write-Nzl "nzl_probe.exe OK: build-nzl\Release\nzl_probe.exe" }
    elseif (Test-Path "build-nzl\nzl_probe.exe") { Write-Nzl "nzl_probe.exe OK: build-nzl\nzl_probe.exe" }
    else { Write-Nzl "nzl_probe.exe NOT built - send me the build errors." }
} else {
    Write-Nzl "cmake not found. Install CMake (winget install Kitware.CMake) + Visual Studio 2022 C++, then rerun."
}
Write-Nzl "Done. Next: load driver (kdmapper, admin) BEFORE game, launch game via Steam, get into match, run nzl_probe.exe, send nzl_probe.txt."
