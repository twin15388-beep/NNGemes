<#
NZL Studio - установка Windows Driver Kit (WDK) под SDK 10.0.26100.
Зачем: для сборки nzl\driver\driver.sys нужны ядерные заголовки km\ntddk.h,
которых нет в обычном Windows SDK (у тебя SDK есть, WDK нет - проверено логом).
Что делает: пробует winget (Microsoft.WindowsWDK.10.0.26100), проверяет
появление ntddk.h, при неудаче winget открывает официальную страницу Microsoft.
Использование (PowerShell от администратора):
  powershell -ExecutionPolicy Bypass -File tools\NZL-InstallWdk.ps1
После установки: перезапуск PowerShell, проверка ntddk.h, пересборка NZL-BuildAll.ps1.
#>
$ErrorActionPreference = "Continue"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }

$ntddk = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km\ntddk.h"
if (Test-Path $ntddk) {
    Write-Nzl "WDK уже установлен: $ntddk"
    Write-Nzl "Просто пересобери: powershell -ExecutionPolicy Bypass -File tools\NZL-BuildAll.ps1"
    exit 0
}
Write-Nzl "ntddk.h не найден - нужен WDK для SDK 10.0.26100."

$winget = Get-Command winget -ErrorAction SilentlyContinue
if ($winget) {
    Write-Nzl "Ставлю через winget: Microsoft.WindowsWDK.10.0.26100 (может занять несколько минут)..."
    & winget install --id Microsoft.WindowsWDK.10.0.26100 --exact --source winget --silent --disable-interactivity --accept-package-agreements --accept-source-agreements 2>&1 | ForEach-Object { Write-Host "  $_" }
    Start-Sleep -Seconds 3
    if (Test-Path $ntddk) {
        Write-Nzl "WDK установлен OK: $ntddk"
        Write-Nzl "Теперь: powershell -ExecutionPolicy Bypass -File tools\NZL-BuildAll.ps1"
        exit 0
    }
    Write-Nzl "winget отработал, но ntddk.h не появился (возможно, нужна другая версия пакета)."
}

Write-Nzl "Ручной вариант (официальный, 2 минуты):"
Write-Nzl "  1. Открой: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk"
Write-Nzl "  2. Скачай WDK installer для Windows 11 24H2 (SDK 10.0.26100) и установи (компоненты по умолчанию)."
Write-Nzl "  Альтернатива: Visual Studio Installer -> Modify -> Individual components -> найди 'Windows Driver Kit'."
Write-Nzl "  3. Проверь в PowerShell:"
Write-Nzl "     Test-Path `"C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km\ntddk.h`"  -> должно быть True"
Write-Nzl "  4. Пересобери: powershell -ExecutionPolicy Bypass -File tools\NZL-BuildAll.ps1"
exit 5
