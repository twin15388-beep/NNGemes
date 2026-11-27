<#
NZL Studio - collector v1 (PowerShell, Windows)
Собирает fingerprint текущего билда ROB RUN: хеши GameAssembly.dll,
UnityPlayer.dll, exe игры и global-metadata.dat + версии файлов.
НЕ читает память процесса, НЕ грузит драйверы, НЕ трогает античит.

Использование (из папки проекта NZL Studio):
  powershell -ExecutionPolicy Bypass -File tools\NZL-Collect.ps1
  powershell -ExecutionPolicy Bypass -File tools\NZL-Collect.ps1 -GameDir "D:\Steam\steamapps\common\ROB RUN"

Если -GameDir не указан, скрипт сам ищет игру: реестр Steam +
libraryfolders.vdf во всех библиотеках Steam.
Результат: папка nzl_info\ (report.json, report.txt) и архив nzl_info.zip рядом.
Весь nzl_info.zip отправь мне.
#>
param(
    [string]$GameDir = "",
    [string]$Out = "nzl_info"
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

function Write-Nzl([string]$msg) { Write-Host "[NZL] $msg" -ForegroundColor Magenta }

function Find-GameDir {
    $candidates = @()
    # 1) Реестр Steam
    $steamPaths = @()
    foreach ($root in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "HKLM:\SOFTWARE\Valve\Steam")) {
        try {
            $p = (Get-ItemProperty -Path $root -ErrorAction Stop).SteamPath
            if ($p) { $steamPaths += $p }
        } catch {}
        try {
            $p2 = (Get-ItemProperty -Path $root -ErrorAction Stop).InstallPath
            if ($p2) { $steamPaths += $p2 }
        } catch {}
    }
    # 2) Стандартные места
    $steamPaths += @(
        "C:\Program Files (x86)\Steam",
        "C:\Program Files\Steam",
        "D:\Steam", "E:\Steam", "D:\SteamLibrary", "E:\SteamLibrary"
    )
    $libraries = @()
    foreach ($sp in ($steamPaths | Select-Object -Unique)) {
        if (-not $sp) { continue }
        $sp = $sp -replace '/', '\'
        if (Test-Path "$sp\steamapps") { $libraries += "$sp\steamapps" }
        $vdf = "$sp\steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            $text = Get-Content $vdf -Raw -ErrorAction SilentlyContinue
            foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
                $lib = $m.Groups[1].Value -replace '\\\\', '\'
                if (Test-Path "$lib\steamapps") { $libraries += "$lib\steamapps" }
            }
        }
    }
    foreach ($apps in ($libraries | Select-Object -Unique)) {
        Get-ChildItem -Path "$apps\common" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            if (Test-Path (Join-Path $_.FullName "GameAssembly.dll")) { $candidates += $_.FullName }
        }
    }
    # 3) Рекурсивный поиск по дискам (медленно, только если выше пусто)
    if ($candidates.Count -eq 0) {
        foreach ($drive in @("C:", "D:", "E:")) {
            if (-not (Test-Path "$drive\")) { continue }
            Get-ChildItem -Path "$drive\" -Filter "GameAssembly.dll" -Recurse -Depth 5 -ErrorAction SilentlyContinue |
                Select-Object -First 5 | ForEach-Object { $candidates += $_.DirectoryName }
            if ($candidates.Count -gt 0) { break }
        }
    }
    return ($candidates | Select-Object -Unique)
}

# --- Выбор папки игры ---
$dirs = @()
if ($GameDir -ne "") {
    if (-not (Test-Path $GameDir)) { Write-Nzl "Папка не найдена: $GameDir"; exit 2 }
    $dirs = @($GameDir)
} else {
    Write-Nzl "Ищу ROB RUN автоматически..."
    $dirs = @(Find-GameDir)
}
if ($dirs.Count -eq 0) {
    Write-Nzl "Игра не найдена автоматически. Запусти с путём:"
    Write-Nzl '  powershell -ExecutionPolicy Bypass -File tools\NZL-Collect.ps1 -GameDir "ПУТЬ_К_ПАПКЕ_ИГРЫ"'
    exit 3
}
$game = $dirs[0]
if ($dirs.Count -gt 1) {
    Write-Nzl "Найдено несколько папок с GameAssembly.dll, беру первую:"
    $dirs | ForEach-Object { Write-Nzl "  $_" }
}
Write-Nzl "Папка игры: $game"

# --- Сбор файлов ---
$wanted = @("GameAssembly.dll", "UnityPlayer.dll", "global-metadata.dat", "ROB RUN.exe", "RobRun.exe", "RUN.exe")
$files = @{}
Get-ChildItem -Path $game -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $wanted -contains $_.Name } |
    ForEach-Object {
        $rel = $_.FullName.Substring($game.Length).TrimStart('\')
        try {
            $hash = (Get-FileHash -Path $_.FullName -Algorithm SHA256).Hash.ToLower()
            $ver = $null
            try { $ver = $_.VersionInfo.FileVersion } catch {}
            $files[$rel] = @{ size = $_.Length; sha256 = $hash; file_version = $ver; full_path = $_.FullName }
        } catch {
            $files[$rel] = @{ error = $_.Exception.Message; full_path = $_.FullName }
        }
    }

# --- Античит / способ запуска (только наличие на диске, ничего не запускаем) ---
$antiCheat = [ordered]@{
    easyanticheat_dir        = (Test-Path (Join-Path $game "EasyAntiCheat"))
    start_protected_game_exe = (Test-Path (Join-Path $game "start_protected_game.exe"))
    sentry_dll               = (Test-Path (Join-Path $game "sentry.dll"))
    crashpad_wer_dll         = (Test-Path (Join-Path $game "crashpad_wer.dll"))
    has_plain_exe            = (Test-Path (Join-Path $game "ROB RUN.exe"))
}
if (Test-Path (Join-Path $game "EasyAntiCheat")) {
    Get-ChildItem -Path (Join-Path $game "EasyAntiCheat") -Recurse -File -ErrorAction SilentlyContinue |
        Select-Object -First 50 | ForEach-Object {
            $rel = $_.FullName.Substring($game.Length).TrimStart('\')
            if (-not $antiCheat.Contains($rel)) { $antiCheat[$rel] = $_.Length }
        }
}

# --- Отчёт ---
$outDir = Join-Path (Get-Location) $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$report = [ordered]@{
    tool        = "NZL Studio collector v1 (PowerShell)"
    date_utc    = (Get-Date).ToUniversalTime().ToString("o")
    computer    = $env:COMPUTERNAME
    user        = $env:USERNAME
    os          = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).Caption
    game_dir    = $game
    anticheat   = $antiCheat
    files       = $files
    need_next   = @(
        "Il2CppDumper: dump.cs, script.json, il2cpp.h (шаг 2, tools\NZL-Dump.ps1)",
        "Версия игры и дата обновления из Steam (Свойства - Установленные файлы) - текстом",
        "Живая проверка в матче - позже, probe-билд NZL (шаг 3)"
    )
}
$report | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $outDir "report.json") -Encoding UTF8

$lines = @("NZL Studio - fingerprint билда", "Дата: $($report.date_utc)", "Папка игры: $game", "")
foreach ($k in $files.Keys) {
    $f = $files[$k]
    if ($f.error) { $lines += "$k -> ERROR: $($f.error)" }
    else { $lines += "$k", "  size: $($f.size)", "  sha256: $($f.sha256)", "  version: $($f.file_version)", "" }
}
$lines | Set-Content -Path (Join-Path $outDir "report.txt") -Encoding UTF8

# --- Архив ---
$zip = Join-Path (Get-Location) "nzl_info.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$outDir\*" -DestinationPath $zip -Force

Write-Nzl "Готово:"
$files.Keys | ForEach-Object {
    $f = $files[$_]
    if (-not $f.error) { Write-Nzl "  $_  size=$($f.size)  sha256=$($f.sha256)" }
}
Write-Nzl "Отправь мне файл: $zip"
if (-not ($files.Keys | Where-Object { $_ -like "*GameAssembly.dll*" })) {
    Write-Nzl "ВНИМАНИЕ: GameAssembly.dll не найден - похоже, указана не та папка."
    exit 4
}
if (-not ($files.Keys | Where-Object { $_ -like "*global-metadata.dat*" })) {
    Write-Nzl "ВНИМАНИЕ: global-metadata.dat не найден - шаг 2 (Il2CppDumper) без него не сделать."
    exit 5
}
exit 0
