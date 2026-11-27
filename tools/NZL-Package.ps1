<#
NZL Studio - portable package builder.
Collects everything another PC needs (no VS/WDK/CMake there):
  NZL-START.bat, tools\*.ps1, nzl\driver\driver.sys (+ kdmapper.exe if
  built), build-nzl\Release\nzl_overlay.exe / nzl_studio.exe / nzl_probe.exe
-> NZL-Studio-Package.zip in the project root. Send that zip; the other
  person unpacks anywhere, runs NZL-START.bat as admin, launches the
  game when told, done.
Usage: powershell -ExecutionPolicy Bypass -File tools\NZL-Package.ps1
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$need = @(
    "NZL-START.bat",
    "build-nzl\Release\nzl_overlay.exe",
    "nzl\driver\driver.sys"
)
foreach ($f in $need) {
    if (-not (Test-Path $f)) { Write-Nzl "FAIL: missing $f - run tools\NZL-BuildAll.ps1 first."; exit 2 }
}
$stage = Join-Path $root "NZL-Studio-Package"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "tools") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "nzl\driver") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "build-nzl\Release") | Out-Null

Copy-Item "NZL-START.bat" $stage -Force
Copy-Item "tools\*.ps1" (Join-Path $stage "tools") -Force
Copy-Item "nzl\driver\driver.sys" (Join-Path $stage "nzl\driver") -Force
if (Test-Path "nzl\driver\kdmapper.exe") {
    Copy-Item "nzl\driver\kdmapper.exe" (Join-Path $stage "nzl\driver") -Force
    Write-Nzl "kdmapper.exe included (recipient needs no Visual Studio)."
} else {
    Write-Nzl "WARNING: kdmapper.exe not found in nzl\driver - recipient PC would need VS to build it. Run tools\NZL-GetKdmapper.ps1 first, then repackage."
}
foreach ($exe in @("nzl_overlay.exe", "nzl_studio.exe", "nzl_probe.exe")) {
    $p = Join-Path "build-nzl\Release" $exe
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage "build-nzl\Release") -Force }
}
$zip = Join-Path $root "NZL-Studio-Package.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Remove-Item $stage -Recurse -Force
Write-Nzl "Package ready: $zip ($([math]::Round((Get-Item $zip).Length/1MB,2)) MB)"
Write-Nzl "Send the zip. Recipient: unpack, NZL-START.bat (admin auto-elevates), launch game when told, INSERT = menu."
exit 0
