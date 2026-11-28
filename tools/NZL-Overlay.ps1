<#
NZL Studio - запуск оверлея ESP (PowerShell от АДМИНИСТРАТОРА).

Игра должна быть уже запущена и находиться в матче, драйвер - загружен.
Оверлей читает процесс через драйвер и рисует поверх окна игры.

Использование:
  powershell -ExecutionPolicy Bypass -File tools\NZL-Overlay.ps1
  powershell -ExecutionPolicy Bypass -File tools\NZL-Overlay.ps1 -SkipDumpCheck

Клавиши: INSERT = меню, END = выход.
Весь вывод пишется в nzl_overlay.txt в корне проекта - его и присылать.
#>
param(
    [switch]$SkipDumpCheck
)

try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Nzl "FAIL: нужен PowerShell ОТ АДМИНИСТРАТОРА (секция драйвера дает win32=5 без админа)."
    exit 2
}

if (-not $SkipDumpCheck) {
    $cd = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl" -ErrorAction SilentlyContinue).CrashDumpEnabled
    if ($cd -ne 2) {
        Write-Nzl "ВНИМАНИЕ: CrashDumpEnabled=$cd, а не 2. Если что-то упадет, стека не будет."
        Write-Nzl "Продолжаю (это оверлей в user mode, драйвер уже загружен). Для полного дампа:"
        Write-Nzl "  powershell -ExecutionPolicy Bypass -File tools\NZL-SetKernelDump.ps1  + перезагрузка"
    }
}

$reg = "HKLM:\SYSTEM\NaCoreFilter"
if (-not (Test-Path $reg)) {
    Write-Nzl "FAIL: драйвер не загружен (HKLM\SYSTEM\NaCoreFilter отсутствует)."
    Write-Nzl "Manual-map драйвер не переживает перезагрузку Windows: после ребута его грузят заново."
    $driverSys = Join-Path $root "nzl\driver\driver.sys"
    if (-not (Test-Path -LiteralPath $driverSys)) {
        Write-Nzl "driver.sys ещё не собран. Сначала:"
        Write-Nzl "  cd `"$root`""
        Write-Nzl "  .\NZL.bat build"
    } else {
        Write-Nzl "Загрузить (окно PowerShell должно быть от администратора), ДО запуска игры:"
        Write-Nzl "  cd `"$root`""
        Write-Nzl "  .\NZL.bat load"
    }
    exit 5
}
$loaded = Get-ItemProperty $reg -ErrorAction SilentlyContinue
if (-not $loaded.Key -or [string]::IsNullOrWhiteSpace([string]$loaded.Shm)) {
    Write-Nzl "FAIL: Key/Shm в реестре пустые - драйвер стартовал не полностью. Перезагрузи и загрузи заново."
    exit 5
}
Write-Nzl "драйвер загружен: Key=$($loaded.Key) Shm=$($loaded.Shm)"

$game = Get-Process "ROB RUN" -ErrorAction SilentlyContinue
if (-not $game) {
    Write-Nzl "FAIL: игра не запущена. Оверлею нечего читать и не над чем рисовать."
    Write-Nzl "Запусти игру через Steam, зайди В МАТЧ, и повтори."
    exit 4
}
Write-Nzl "игра запущена: pid=$($game.Id)"

$exe = Join-Path $root "build-nzl\Release\nzl_overlay.exe"
if (-not (Test-Path $exe)) {
    Write-Nzl "FAIL: nzl_overlay.exe не найден. Сначала: tools\NZL-BuildAll.ps1"
    exit 3
}

# A layered window cannot draw over exclusive fullscreen: DirectX flips the
# whole display and the desktop composition is bypassed. Borderless/windowed
# is not a preference here, it is the only mode where the ESP is visible.
Write-Nzl "Проверь в игре: режим экрана = Windowed / Borderless. В exclusive fullscreen оверлей не виден."
Write-Nzl "Клавиши: INSERT = меню, END = выход."

$log = Join-Path $root "nzl_overlay.txt"
"NZL overlay $(Get-Date -Format o) pid_game=$($game.Id) driver_key=$($loaded.Key)" |
    Set-Content $log -Encoding UTF8

Push-Location (Split-Path $exe -Parent)
try {
    & $exe *>&1 | ForEach-Object {
        Write-Host $_
        $_ | Add-Content $log -Encoding UTF8
    }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

Write-Nzl "оверлей завершился: exit=$code"
Write-Nzl "лог: $log"
Write-Nzl "Дальше: powershell -ExecutionPolicy Bypass -File tools\NZL-Report.ps1 -Zip -CopyToDropbox"
exit 0
