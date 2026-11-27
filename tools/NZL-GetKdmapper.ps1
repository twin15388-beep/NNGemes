<#
NZL Studio - получить kdmapper.exe (Windows). У TheCruZ/kdmapper нет готовых
релизов - собираем из исходников твоим Visual Studio в D:\kdmapper-src,
готовый exe копируем в nzl\driver\kdmapper.exe.
Запуск (PowerShell, обычный, не обязательно админ):
  powershell -ExecutionPolicy Bypass -File tools\NZL-GetKdmapper.ps1
Альтернатива: если у тебя уже есть kd2.exe от старого пакета (kdmapper под
другим именем), можно не собирать:
  powershell -ExecutionPolicy Bypass -File tools\NZL-LoadDriver.ps1 -Kdmapper "ПУТЬ\kd2.exe"
#>
$ErrorActionPreference = "Continue"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent
$dst = Join-Path $root "nzl\driver\kdmapper.exe"
if (Test-Path $dst) { Write-Nzl "kdmapper.exe уже на месте: $dst"; exit 0 }

$src = "D:\kdmapper-src"
if (-not (Test-Path (Join-Path $src "kdmapper.sln"))) {
    Write-Nzl "Клонирую TheCruZ/kdmapper в $src ..."
    & git clone --depth 1 https://github.com/TheCruZ/kdmapper.git $src 2>&1 | ForEach-Object { Write-Host "  $_" }
}
if (-not (Test-Path (Join-Path $src "kdmapper.sln"))) { Write-Nzl "FAIL: нет kdmapper.sln. Пришли вывод git clone."; exit 2 }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) { $vs = "D:\programs\vs" }
$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) { Write-Nzl "FAIL: MSBuild не найден: $msbuild. Пришли этот текст."; exit 3 }
Write-Nzl "MSBuild: $msbuild"

Write-Nzl "Собираю kdmapper (Release x64)..."
Push-Location $src
& $msbuild kdmapper.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m /nologo /v:minimal 2>&1 | ForEach-Object { Write-Host "  $_" }
Pop-Location
$exe = Get-ChildItem -Path $src -Filter "kdmapper*.exe" -Recurse -ErrorAction SilentlyContinue | Sort-Object Length -Descending | Select-Object -First 1
if (-not $exe) {
    Write-Nzl "FAIL: kdmapper.exe после сборки не найден. Пришли последние 30 строк вывода MSBuild выше."
    Write-Nzl "Частая причина: старый toolset в .sln. Тогда открой kdmapper.sln в Visual Studio, Retarget solution, собери Release x64 вручную и скопируй exe в nzl\driver\kdmapper.exe."
    exit 4
}
Copy-Item $exe.FullName $dst -Force
Write-Nzl "kdmapper.exe OK: $dst"
Write-Nzl "Дальше (админ, игра выключена): powershell -ExecutionPolicy Bypass -File tools\NZL-LoadDriver.ps1"
