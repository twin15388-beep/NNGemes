<#
NZL Studio - загрузка Sharedmem Driver (Windows, PowerShell от АДМИНИСТРАТОРА).
Делает: если драйвер уже есть в этой сессии — использует его повторно;
иначе проверяет driver.sys/kdmapper, загружает и проверяет HKLM\SYSTEM\NaCoreFilter.
Первичную загрузку делай до старта игры. Ручной kdmapper-драйвер не выгружается:
перезагрузка нужна только для его замены/снятия, а не при каждом запуске overlay.

Использование (из корня проекта NZL, PowerShell от администратора):
  powershell -ExecutionPolicy Bypass -File tools\NZL-LoadDriver.ps1
  powershell -ExecutionPolicy Bypass -File tools\NZL-LoadDriver.ps1 -Kdmapper "C:\kdmapper\kdmapper.exe"
kdmapper.exe по умолчанию ищется в: nzl\driver\kdmapper.exe, .\kdmapper.exe,
C:\kdmapper\kdmapper.exe, %USERPROFILE%\Downloads\kdmapper.exe
#>
param(
    [string]$Kdmapper = "",
    [string]$DriverSys = ""
)

$ErrorActionPreference = "Continue"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent

# --- Администратор обязателен ---
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Nzl "FAIL: PowerShell НЕ от администратора. Закрой, запусти PowerShell от администратора и повтори."
    exit 2
}
Write-Nzl "Администратор: OK"

# A kdmapper image has no safe service-manager unload path. Reuse the
# current transport across overlay/game launches instead of mapping a
# second copy (which has caused crashes in the past).
$reg = "HKLM:\SYSTEM\NaCoreFilter"
if (Test-Path $reg) {
    $loaded = Get-ItemProperty $reg -ErrorAction SilentlyContinue
    if ($loaded.Key -and -not [string]::IsNullOrWhiteSpace([string]$loaded.Shm)) {
        Write-Nzl "Драйвер уже загружен в этой сессии (Key=$($loaded.Key), Shm=$($loaded.Shm))."
        Write-Nzl "Повторно kdmapper не запускаю: overlay можно открывать без перезагрузки."
        Write-Nzl "Перезагрузка нужна только для замены/снятия вручную отображённого driver.sys."
        exit 0
    }
    Write-Nzl "FAIL: HKLM\SYSTEM\NaCoreFilter существует, но Key/Shm неполные."
    Write-Nzl "Не загружаю второй экземпляр поверх возможного остатка драйвера; сначала перезагрузи Windows."
    exit 8
}

# --- driver.sys ---
if ($DriverSys -eq "") { $DriverSys = Join-Path $root "nzl\driver\driver.sys" }
if (-not (Test-Path $DriverSys)) {
    Write-Nzl "FAIL: driver.sys не найден: $DriverSys"
    Write-Nzl "Сначала собери: powershell -ExecutionPolicy Bypass -File tools\NZL-BuildAll.ps1"
    exit 3
}
$drvInfo = Get-Item $DriverSys
Write-Nzl "driver.sys: $($drvInfo.FullName) size=$($drvInfo.Length)"

# --- kdmapper ---
if ($Kdmapper -eq "") {
    foreach ($c in @(
        (Join-Path $root "nzl\driver\kdmapper.exe"),
        (Join-Path $root "kdmapper.exe"),
        "C:\kdmapper\kdmapper.exe",
        "$env:USERPROFILE\Downloads\kdmapper.exe"
    )) { if (Test-Path $c) { $Kdmapper = $c; break } }
}
if ($Kdmapper -eq "" -or -not (Test-Path $Kdmapper)) {
    Write-Nzl "FAIL: kdmapper.exe не найден. Скачай kdmapper (TheCruZ/kdmapper, GitHub, последний release),"
    Write-Nzl "положи в nzl\driver\kdmapper.exe и повтори. Или укажи: -Kdmapper `"C:\путь\kdmapper.exe`""
    exit 4
}
Write-Nzl "kdmapper: $Kdmapper"

# --- Предохранитель после двух BSOD: нужен kernel dump для настоящего стека ---
$cd = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl" -ErrorAction SilentlyContinue).CrashDumpEnabled
if ($cd -ne 2) {
    Write-Nzl "FAIL: CrashDumpEnabled=$cd, нужен 2 (kernel dump). Без него третий краш опять даст пустой стек."
    Write-Nzl "Выполни: powershell -ExecutionPolicy Bypass -File tools\NZL-SetKernelDump.ps1, затем Restart-Computer, и только потом загрузка драйвера."
    exit 9
}
Write-Nzl "Kernel dump включён (CrashDumpEnabled=2): OK"

# --- Повторная проверка перед kdmapper: не маппить второй экземпляр ---
if (Test-Path $reg) {
    $old = Get-ItemProperty $reg -ErrorAction SilentlyContinue
    if ($old.Key -and -not [string]::IsNullOrWhiteSpace([string]$old.Shm)) {
        Write-Nzl "Драйвер уже загружен параллельным запуском (Key=$($old.Key), Shm=$($old.Shm)); повторное отображение пропущено."
        exit 0
    }
    Write-Nzl "FAIL: появилось неполное состояние HKLM\SYSTEM\NaCoreFilter; второй экземпляр загружать небезопасно."
    exit 8
}

# --- Игра должна быть выключена на момент загрузки ---
$game = Get-Process "ROB RUN" -ErrorAction SilentlyContinue
if ($game) {
    Write-Nzl "ВНИМАНИЕ: игра уже запущена (pid $($game.Id)). Правильный порядок: сначала драйвер, потом игра."
    Write-Nzl "Закрой игру, загрузи драйвер этим скриптом, потом запускай игру через Steam."
    exit 5
}

# --- Среда: Secure Boot / HVCI (информативно, не блокируем) ---
try {
    $sb = Confirm-SecureBootUEFI -ErrorAction Stop
    Write-Nzl "Secure Boot: $sb (если kdmapper упадёт - это одна из причин)"
} catch { Write-Nzl "Secure Boot: проверить не удалось ($($_.Exception.Message))" }
try {
    $dg = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction Stop
    Write-Nzl "VBS/HVCI status: $($dg.VirtualizationBasedSecurityStatus) (2 = работает, может мешать kdmapper)"
} catch {}

# --- Загрузка ---
Write-Nzl "Загружаю драйвер (kdmapper)..."
& $Kdmapper $DriverSys 2>&1 | ForEach-Object { Write-Host "  $_" }
$code = $LASTEXITCODE
Start-Sleep -Seconds 2

# --- Проверка реестра (драйвер публикует Key + Shm при старте) ---
$reg = "HKLM:\SYSTEM\NaCoreFilter"
if (-not (Test-Path $reg)) {
    Write-Nzl "FAIL: HKLM\SYSTEM\NaCoreFilter не появился (kdmapper exit=$code)."
    Write-Nzl "Пришли мне весь вывод kdmapper выше дословно."
    exit 6
}
$props = Get-ItemProperty -Path $reg
Write-Nzl "Реестр: Key=$($props.Key) Shm=$($props.Shm)"
if (-not $props.Key -or -not $props.Shm) {
    Write-Nzl "FAIL: реестр есть, но Key/Shm пустые - драйвер стартовал не полностью. Пришли вывод выше."
    exit 7
}
Write-Nzl "TRANSPORT_LOADED: sharedmem-драйвер жив, ключ опубликован."
Write-Nzl "Дальше: 1) запусти игру через Steam, зайди в матч; 2) выполни:"
Write-Nzl '  "D:\NZL Rob run\build-nzl\Release\nzl_probe.exe"'
Write-Nzl "3) пришли файл nzl_probe.txt оттуда, где запускал probe."
Write-Nzl "Повторный запуск overlay не требует перезагрузки: драйвер останется загруженным."
Write-Nzl "Перезагружай Windows только если нужно заменить/снять этот driver.sys или транспорт завис."
exit 0
