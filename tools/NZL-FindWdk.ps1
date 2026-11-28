<#
NZL Studio - диагностика WDK (Windows). Ищет ntddk.h по всем Windows Kits
и печатает, где реально лежат заголовки. Запуск:
  powershell -ExecutionPolicy Bypass -File tools\NZL-FindWdk.ps1
Вывод целиком пришли мне, если ntddk.h не находится.
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
Write-Nzl "Реестр Windows Kits:"
foreach ($k in @("HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots", "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots")) {
    if (Test-Path $k) { (Get-ItemProperty $k).PSObject.Properties | Where-Object { $_.Name -like "KitsRoot*" } | ForEach-Object { Write-Nzl "  $($_.Name) = $($_.Value)" } }
}
Write-Nzl "Поиск ntddk.h (может занять до минуты)..."
$found = @()
foreach ($root in @("C:\Program Files (x86)\Windows Kits", "C:\Program Files\Windows Kits", "D:\Windows Kits", "D:\Program Files (x86)\Windows Kits")) {
    if (Test-Path $root) {
        Get-ChildItem -Path $root -Filter "ntddk.h" -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $found += $_.FullName }
    }
}
if ($found) { $found | ForEach-Object { Write-Nzl "  FOUND: $_" } }
else {
    Write-Nzl "  ntddk.h НЕ найден ни в одном Windows Kits."
    Write-Nzl "  Папки Include, которые есть:"
    foreach ($root in @("C:\Program Files (x86)\Windows Kits\10\Include", "C:\Program Files\Windows Kits\10\Include")) {
        if (Test-Path $root) { Get-ChildItem $root -Directory | ForEach-Object { Write-Nzl "    $($_.FullName)" } }
    }
    Write-Nzl "  winget мог поставить WDK без компонентов заголовков. Поставь через Visual Studio Installer:"
    Write-Nzl "  Modify -> Individual components -> 'Windows Driver Kit' + 'Windows SDK 10.0.26100'."
}
