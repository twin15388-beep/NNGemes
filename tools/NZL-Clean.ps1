<#
NZL Studio - чистка рабочей папки.

По умолчанию НИЧЕГО НЕ УДАЛЯЕТ: только показывает, что удалил бы и сколько
места это освободит. Удаляет только с -Apply.

  powershell -NoProfile -ExecutionPolicy Bypass -File tools\NZL-Clean.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\NZL-Clean.ps1 -Apply
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\NZL-Clean.ps1 -Apply -KeepDumps

Что удаляется (всё это воспроизводимо одной сборкой):
  build-nzl\                 дерево CMake/MSVC
  build\  cpp\build\         старые деревья
  .vs\                       кеш Visual Studio
  NZL-Studio*.bat            упакованные лаунчеры прошлых сборок
  NZL-Studio-Package.zip
  driver_build.log  build_all.log  nzl_probe.txt  nzl_overlay.txt
  crash_info\                разборы BSOD (дампы тоже, если не -KeepDumps)
  nzl_report_*.txt/.zip      отчёты прошлых прогонов

Что НЕ трогается никогда:
  исходники, HANDOFF.md, docs\, data\, tools\
  requests.jsonl и 20261126T*.json  (аналитические копии, удаляются вручную)
  driver.sys в nzl\driver\   (это артефакт сборки, но 18 КБ, руками не мешает)
#>
param(
    [switch]$Apply,
    [switch]$KeepDumps
)

try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent

function Get-SizeOf([string]$path) {
    if (-not (Test-Path $path)) { return 0 }
    $item = Get-Item $path -Force
    if ($item.PSIsContainer) {
        $sum = (Get-ChildItem $path -Recurse -Force -File -ErrorAction SilentlyContinue |
                Measure-Object -Property Length -Sum).Sum
        if ($sum) { return [int64]$sum }
        return 0
    }
    return [int64]$item.Length
}

$dirs = @("build-nzl", "build", "cpp\build", ".vs")
$files = @("NZL-Studio-Package.zip", "driver_build.log", "build_all.log",
           "nzl_probe.txt", "nzl_overlay.txt")

$targets = New-Object System.Collections.Generic.List[object]
foreach ($d in $dirs) {
    $p = Join-Path $root $d
    if (Test-Path $p) { $targets.Add([pscustomobject]@{ Path = $p; Kind = "dir"; Size = (Get-SizeOf $p) }) }
}
foreach ($f in $files) {
    $p = Join-Path $root $f
    if (Test-Path $p) { $targets.Add([pscustomobject]@{ Path = $p; Kind = "file"; Size = (Get-SizeOf $p) }) }
}
Get-ChildItem $root -Filter "NZL-Studio*.bat" -File -ErrorAction SilentlyContinue |
    ForEach-Object { $targets.Add([pscustomobject]@{ Path = $_.FullName; Kind = "file"; Size = [int64]$_.Length }) }
Get-ChildItem $root -Filter "nzl_report_*" -File -ErrorAction SilentlyContinue |
    ForEach-Object { $targets.Add([pscustomobject]@{ Path = $_.FullName; Kind = "file"; Size = [int64]$_.Length }) }

$crash = Join-Path $root "crash_info"
if (Test-Path $crash) {
    $crashSize = Get-SizeOf $crash
    if ($KeepDumps) {
        $dmps = Get-ChildItem $crash -Filter "*.dmp" -File -ErrorAction SilentlyContinue
        foreach ($d in $dmps) {
            $targets.Add([pscustomobject]@{ Path = $d.FullName; Kind = "file"; Size = [int64]$d.Length })
        }
        Get-ChildItem $crash -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -ne ".dmp" } |
            ForEach-Object { $targets.Add([pscustomobject]@{ Path = $_.FullName; Kind = "file"; Size = [int64]$_.Length }) }
        Write-Nzl "-KeepDumps: .dmp файлы останутся в crash_info\, удалю только тексты разбора."
    } else {
        $targets.Add([pscustomobject]@{ Path = $crash; Kind = "dir"; Size = $crashSize })
    }
}

if ($targets.Count -eq 0) {
    Write-Nzl "Чистить нечего: рабочая папка уже пустая в смысле артефактов."
    exit 0
}

$total = ($targets | Measure-Object -Property Size -Sum).Sum
Write-Nzl ("Найдено {0} объектов, {1:N1} МБ:" -f $targets.Count, ($total / 1MB))
foreach ($t in ($targets | Sort-Object -Property Size -Descending)) {
    $rel = $t.Path.Substring($root.Length).TrimStart('\')
    Write-Host ("  {0,10:N1} МБ  {1}" -f ($t.Size / 1MB), $rel)
}

if (-not $Apply) {
    Write-Nzl "Это был пробный прогон. Ничего не удалено."
    Write-Nzl "Удалить: tools\NZL-Clean.ps1 -Apply"
    exit 0
}

$failed = 0
foreach ($t in $targets) {
    try {
        Remove-Item -LiteralPath $t.Path -Recurse -Force -ErrorAction Stop
    } catch {
        $failed++
        Write-Nzl "не удалось удалить $($t.Path): $($_.Exception.Message)"
    }
}
$left = $targets | Where-Object { Test-Path $_.Path }
Write-Nzl ("Удалено {0} из {1}, освобождено {2:N1} МБ." -f ($targets.Count - $failed), $targets.Count, ($total / 1MB))
if ($left) {
    Write-Nzl "Осталось (занято другим процессом - закрой VS/игру/оверлей и повтори):"
    foreach ($l in $left) { Write-Host "  $($l.Path)" }
}
Write-Nzl "Дальше: .\NZL.bat build  (пересоберёт всё с нуля, 1-2 минуты)"
