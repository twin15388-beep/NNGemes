<#
NZL Studio - build everything (Windows, run from the project root).
Builds the MSVC+WDK driver, all three x64 NZL applications, then runs the
host-side C++ tests. A non-zero process exit means at least one requested
stage failed; per-stage exit codes are printed above.
Usage:
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\NZL-BuildAll.ps1
#>
$ErrorActionPreference = "Continue"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$message) {
    Write-Host "[NZL] $message" -ForegroundColor Magenta
}

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$overallExit = 0

Write-Nzl "1/3 driver (MSVC+WDK)..."
$driverLog = Join-Path $root "driver_build.log"
$driverPath = Join-Path $root "nzl\driver\driver.sys"
Push-Location (Join-Path $root "nzl\driver")
try {
    & cmd /c "build_driver.bat > `"$driverLog`" 2>&1"
    $driverExit = $LASTEXITCODE
} finally {
    Pop-Location
}
if (Test-Path $driverLog) {
    Get-Content $driverLog | ForEach-Object { Write-Host "  $_" }
}
$driverLogOk = (Test-Path $driverLog) -and (Select-String -Path $driverLog -SimpleMatch "BUILD_OK" -Quiet -ErrorAction SilentlyContinue)
if ($driverExit -eq 0 -and $driverLogOk -and (Test-Path $driverPath)) {
    Write-Nzl "driver.sys OK: $driverPath (exit=$driverExit)"
} else {
    Write-Nzl "driver.sys NOT built (exit=$driverExit); existing driver.sys, if any, was left untouched."
    $overallExit = 1
}

Write-Nzl "2/3 NZL x64 applications..."
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCommand) {
    Write-Nzl "cmake not found. Install CMake and Visual Studio 2022 C++, then rerun."
    $overallExit = 1
} else {
    $configureArgs = @("-S", "nzl", "-B", "build-nzl", "-A", "x64")
    & $cmakeCommand.Source @configureArgs
    $configureExit = $LASTEXITCODE
    Write-Nzl "CMake configure exit=$configureExit"
    if ($configureExit -ne 0) {
        $overallExit = 1
    } else {
        & $cmakeCommand.Source --build build-nzl --config Release
        $buildExit = $LASTEXITCODE
        Write-Nzl "CMake build exit=$buildExit"
        if ($buildExit -ne 0) {
            $overallExit = 1
        } else {
            $expected = @{
                "nzl_probe.exe" = @("build-nzl\Release\nzl_probe.exe", "build-nzl\nzl_probe.exe")
                "nzl_studio.exe" = @("build-nzl\Release\nzl_studio.exe", "build-nzl\nzl_studio.exe")
                "nzl_overlay.exe" = @("build-nzl\Release\nzl_overlay.exe", "build-nzl\nzl_overlay.exe")
            }
            foreach ($name in $expected.Keys) {
                $found = $false
                foreach ($candidate in $expected[$name]) {
                    if (Test-Path $candidate) {
                        Write-Nzl "$name OK: $candidate"
                        $found = $true
                        break
                    }
                }
                if (-not $found) {
                    Write-Nzl "$name NOT FOUND after a successful CMake build."
                    $overallExit = 1
                }
            }
        }
    }
}

Write-Nzl "3/3 host unit tests..."
if ($cmakeCommand) {
    $ctestCommand = Get-Command ctest -ErrorAction SilentlyContinue
    if (-not $ctestCommand) {
        Write-Nzl "ctest not found; test stage could not run."
        $overallExit = 1
    } elseif ($configureExit -eq 0 -and $buildExit -eq 0) {
        & $ctestCommand.Source --test-dir build-nzl -C Release --output-on-failure
        $testExit = $LASTEXITCODE
        Write-Nzl "CTest exit=$testExit"
        if ($testExit -ne 0) { $overallExit = 1 }
    } else {
        Write-Nzl "CTest skipped because configure/build failed."
        $overallExit = 1
    }
} else {
    Write-Nzl "CTest skipped because CMake is unavailable."
}

Write-Nzl "Build workflow exit=$overallExit. Next run NZL-START.bat; it reuses a complete Key/Shm state and will not map the driver twice."
exit $overallExit
