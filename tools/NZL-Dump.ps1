<#
NZL Studio - шаг 2: Il2CppDumper (PowerShell, Windows)
Запускает Il2CppDumper по файлам ТВОЕГО текущего билда и складывает
результат в nzl_dump\ (dump.cs, script.json, il2cpp.h + копия report).
НЕ трогает процесс игры и античит - работает только с файлами на диске.

Перед запуском:
  1. Сначала выполни шаг 1 (tools\NZL-Collect.ps1) - он найдёт папку игры.
  2. Скачай Il2CppDumper (Perfare/Il2CppDumper, последний release) с GitHub
     и распакуй, например в C:\Il2CppDumper\ (там должен быть Il2CppDumper.exe).

Использование (из папки проекта NZL Studio):
  powershell -ExecutionPolicy Bypass -File tools\NZL-Dump.ps1 -DumperDir "C:\Il2CppDumper"
  powershell -ExecutionPolicy Bypass -File tools\NZL-Dump.ps1 -DumperDir "C:\Il2CppDumper" -GameDir "D:\Steam\steamapps\common\ROB RUN"

Результат: nzl_dump.zip - весь отправь мне.
#>
param(
    [string]$GameDir = "",
    [string]$DumperDir = "",
    [string]$Out = "nzl_dump"
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$msg) { Write-Host "[NZL] $msg" -ForegroundColor Magenta }

# --- Папка игры: аргумент или report.json шага 1 ---
if ($GameDir -eq "") {
    $rep = Join-Path (Get-Location) "nzl_info\report.json"
    if (Test-Path $rep) {
        try { $GameDir = (Get-Content $rep -Raw | ConvertFrom-Json).game_dir } catch {}
    }
}
if ($GameDir -eq "" -or -not (Test-Path $GameDir)) {
    Write-Nzl "Не знаю папку игры. Сначала шаг 1 (NZL-Collect.ps1) или укажи -GameDir."
    exit 2
}
$ga = Join-Path $GameDir "GameAssembly.dll"
if (-not (Test-Path $ga)) {
    # GameAssembly.dll может лежать не в корне - ищем рекурсивно
    $found = Get-ChildItem -Path $GameDir -Filter "GameAssembly.dll" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $ga = $found.FullName }
}
if (-not (Test-Path $ga)) { Write-Nzl "GameAssembly.dll не найден в $GameDir"; exit 3 }
$meta = Get-ChildItem -Path $GameDir -Filter "global-metadata.dat" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $meta) {
    Write-Nzl "global-metadata.dat не найден. Обычно: ROB RUN_Data\il2cpp_data\Metadata\global-metadata.dat"
    exit 4
}
Write-Nzl "GameAssembly: $ga"
Write-Nzl "Metadata:     $($meta.FullName)"

# --- Il2CppDumper ---
if ($DumperDir -eq "") {
    foreach ($c in @("C:\Il2CppDumper", "$env:USERPROFILE\Downloads\Il2CppDumper", ".\Il2CppDumper")) {
        if (Test-Path (Join-Path $c "Il2CppDumper.exe")) { $DumperDir = $c; break }
    }
}
$dumper = Join-Path $DumperDir "Il2CppDumper.exe"
if ($DumperDir -eq "" -or -not (Test-Path $dumper)) {
    Write-Nzl "Il2CppDumper.exe не найден. Скачай Perfares Il2CppDumper (GitHub, последний release),"
    Write-Nzl "распакуй в C:\Il2CppDumper\ и запусти:"
    Write-Nzl '  powershell -ExecutionPolicy Bypass -File tools\NZL-Dump.ps1 -DumperDir "C:\Il2CppDumper"'
    exit 5
}

# --- Запуск дампера ---
$outDir = Join-Path (Get-Location) $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Write-Nzl "Запускаю Il2CppDumper (может занять несколько минут)..."
Push-Location $outDir
try {
    & $dumper $ga $meta.FullName $outDir 2>&1 | Tee-Object -FilePath (Join-Path $outDir "dumper.log")
    $code = $LASTEXITCODE
} finally { Pop-Location }
if ($code -ne 0) {
    Write-Nzl "Il2CppDumper завершился с кодом $code. Пришли мне ВЕСЬ текст из dumper.log (папка $outDir)."
    Write-Nzl "И первые 64 байта metadata hex:"
    Write-Nzl "  Format-Hex -Path `"$($meta.FullName)`" -Count 64"
    exit 6
}

# --- Проверка результата и архив ---
$need = @("dump.cs", "script.json", "il2cpp.h")
$missing = $need | Where-Object { -not (Test-Path (Join-Path $outDir $_)) }
if ($missing) {
    Write-Nzl "Дампер отработал, но не хватает: $($missing -join ', '). Смотри $outDir\dumper.log и пришли его мне."
    exit 7
}
$repSrc = Join-Path (Get-Location) "nzl_info\report.json"
if (Test-Path $repSrc) { Copy-Item $repSrc (Join-Path $outDir "report.json") -Force }
$zip = Join-Path (Get-Location) "nzl_dump.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$outDir\*" -DestinationPath $zip -Force
Write-Nzl "Готово. Отправь мне файл: $zip"
$need | ForEach-Object {
    $p = Join-Path $outDir $_
    Write-Nzl "  $_  size=$((Get-Item $p).Length)"
}
exit 0
