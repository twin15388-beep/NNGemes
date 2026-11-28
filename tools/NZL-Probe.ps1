<#
NZL Studio - запуск probe одной командой (PowerShell от АДМИНИСТРАТОРА).
Игра должна быть запущена и находиться в меню/матче.
Использование:
  powershell -ExecutionPolicy Bypass -File tools\NZL-Probe.ps1
Что делает: проверка админа -> запуск build-nzl\Release\nzl_probe.exe ->
печать nzl_probe.txt. Файл nzl_probe.txt пришли мне.
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Nzl "FAIL: нужен PowerShell ОТ АДМИНИСТРАТОРА (секция драйвера дает win32=5 без админа)."
    exit 2
}
# Driver first: a manual-mapped driver does not survive a reboot, so after any
# restart this is the most likely reason a probe reads nothing.
$reg = "HKLM:\SYSTEM\NaCoreFilter"
if (-not (Test-Path $reg)) {
    Write-Nzl "FAIL: драйвер не загружен (HKLM\SYSTEM\NaCoreFilter отсутствует)."
    Write-Nzl "После перезагрузки manual-map драйвер НЕ выживает. Сначала, от администратора:"
    Write-Nzl "  cd `"$root`""
    Write-Nzl "  .\NZL.bat load"
    exit 5
}
$loaded = Get-ItemProperty $reg -ErrorAction SilentlyContinue
if (-not $loaded.Key -or [string]::IsNullOrWhiteSpace([string]$loaded.Shm)) {
    Write-Nzl "FAIL: ключ HKLM\SYSTEM\NaCoreFilter есть, но Key/Shm пустые - драйвер стартовал не полностью."
    Write-Nzl "Перезагрузи Windows и загрузи драйвер заново: tools\NZL-LoadDriver.ps1"
    exit 5
}
Write-Nzl "драйвер загружен: Key=$($loaded.Key) Shm=$($loaded.Shm)"

# The probe reads the live process; without the game there is nothing to read.
$game = Get-Process "ROB RUN" -ErrorAction SilentlyContinue
if (-not $game) {
    Write-Nzl "FAIL: игра не запущена. probe читает ЖИВОЙ процесс, без игры проверять нечего."
    Write-Nzl "Запусти игру через Steam, зайди В МАТЧ (не меню), и повтори эту же команду."
    exit 4
}
Write-Nzl "игра запущена: pid=$($game.Id) - запускаю probe"

$exe = Join-Path $root "build-nzl\Release\nzl_probe.exe"
if (-not (Test-Path $exe)) {
    Write-Nzl "FAIL: nzl_probe.exe не найден. Сначала: tools\NZL-BuildAll.ps1"
    exit 3
}
$dir = Split-Path $exe -Parent
Push-Location $dir
try {
    & $exe
    $code = $LASTEXITCODE
} finally { Pop-Location }
$txt = Join-Path $dir "nzl_probe.txt"
Write-Nzl "probe exit=$code"
if (Test-Path $txt) {
    Write-Nzl "--- nzl_probe.txt ---"
    Get-Content $txt | ForEach-Object { Write-Host $_ }
    Write-Nzl "Пришли мне файл: $txt"
} else {
    Write-Nzl "nzl_probe.txt не создан - пришли вывод консоли дословно."
}
