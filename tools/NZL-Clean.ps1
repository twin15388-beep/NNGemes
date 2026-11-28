<#
NZL Studio - чистка рабочей папки.

По умолчанию ничего не делает: печатает, что убрал бы, по категориям.

  .\NZL.bat clean                  пробный прогон
  .\NZL.bat clean -Apply           перенести мусор в _trash-<дата> (можно вернуть)
  .\NZL.bat clean -Apply -Delete   удалить сразу, без корзины
  .\NZL.bat clean -Apply -KeepDumps  дампы BSOD оставить в crash_info\

Почему корзина, а не удаление: перенос мгновенный и обратимый, а удаление
1.5 ГБ старых сборок необратимо. Осмотрел _trash-<дата> - удали папку целиком.

Никогда не трогает: nzl\, tools\, tests\, docs\, NZL.bat, README.md,
HANDOFF.md, config.ini, .gitattributes, .gitignore, .git\, а также файлы под
контролем git (robrun.py, RobRun.exe, START.bat, NZL-START.bat): их удаление
сломало бы рабочую копию, для них есть отдельное решение.
#>
param(
    [switch]$Apply,
    [switch]$Delete,
    [switch]$KeepDumps
)

try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
function Write-Dim([string]$m) { Write-Host "      $m" -ForegroundColor DarkGray }

$root = Split-Path $PSScriptRoot -Parent
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$trash = Join-Path $root ("_trash-" + $stamp)

function Get-SizeOf([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return [int64]0 }
    $item = Get-Item -LiteralPath $path -Force
    if ($item.PSIsContainer) {
        $sum = (Get-ChildItem -LiteralPath $path -Recurse -Force -File -ErrorAction SilentlyContinue |
                Measure-Object -Property Length -Sum).Sum
        if ($sum) { return [int64]$sum }
        return [int64]0
    }
    return [int64]$item.Length
}

function Resolve-Pattern([string]$pattern) {
    $found = New-Object System.Collections.Generic.List[object]
    $full = Join-Path $root $pattern
    if ($pattern -match "[?*]") {
        $parentPart = Split-Path $pattern -Parent
        if ([string]::IsNullOrEmpty($parentPart)) {
            $parent = $root
        } else {
            $parent = Join-Path $root $parentPart
        }
        if (Test-Path -LiteralPath $parent) {
            $leaf = Split-Path $pattern -Leaf
            Get-ChildItem -LiteralPath $parent -Filter $leaf -Force -ErrorAction SilentlyContinue |
                ForEach-Object { $found.Add($_) }
        }
    } elseif (Test-Path -LiteralPath $full) {
        $found.Add((Get-Item -LiteralPath $full -Force))
    }
    return $found
}

# Категории перечислены явно и узко: ни одно правило не может случайно
# захватить исходники, потому что шаблоны начинаются с конкретных имён.
$groups = @(
    @{ Name = "Старые сборки";                  Items = @("build", "build-nzl", "build-nzl-old-*", "cpp\build", ".vs") },
    @{ Name = "Логи сборки";                    Items = @("driver_build.log", "build_all.log", "driver\_build*.log") },
    @{ Name = "Отчёты и пробы";                 Items = @("nzl_report_*", "nzl_probe*.txt", "nzl_overlay.txt", "nzl_probe", "nzl_info", "nzl_info.zip") },
    @{ Name = "Разборы падений";                Items = @("crash_info") },
    @{ Name = "Артефакты старого локального API"; Items = @("2026*T*.json", "requests.jsonl") },
    @{ Name = "Упакованные лаунчеры и черновики"; Items = @("NZL-Studio*.bat", "NZL-Studio-Package.zip", "disasm.txt") },
    @{ Name = "Прошлые корзины";                Items = @("_trash-*") }
)

$seen = New-Object System.Collections.Generic.HashSet[string]
$rows = New-Object System.Collections.Generic.List[object]
foreach ($group in $groups) {
    foreach ($pattern in $group.Items) {
        foreach ($item in (Resolve-Pattern $pattern)) {
            if (-not $seen.Add($item.FullName)) { continue }
            if ($KeepDumps -and $item.PSIsContainer -and $item.Name -eq "crash_info") {
                Write-Nzl "-KeepDumps: crash_info\ оставлен, дампы не отправляются в корзину."
                continue
            }
            $rows.Add([pscustomobject]@{
                Category = $group.Name
                Path     = $item.FullName
                Name     = $item.Name
                IsDir    = [bool]$item.PSIsContainer
                Size     = (Get-SizeOf $item.FullName)
            })
        }
    }
}

if ($rows.Count -eq 0) {
    Write-Nzl "Мусора не найдено: папка уже чистая."
} else {
    $totalMb = (($rows | Measure-Object -Property Size -Sum).Sum) / 1MB
    Write-Nzl ("Мусор: {0} объектов, {1:N1} МБ" -f $rows.Count, $totalMb)
    foreach ($group in $groups) {
        $inGroup = @($rows | Where-Object { $_.Category -eq $group.Name })
        if ($inGroup.Count -eq 0) { continue }
        $groupMb = (($inGroup | Measure-Object -Property Size -Sum).Sum) / 1MB
        Write-Host ("  {0}  ({1} шт., {2:N1} МБ)" -f $group.Name, $inGroup.Count, $groupMb) -ForegroundColor Cyan
        $shown = 0
        foreach ($row in ($inGroup | Sort-Object -Property Size -Descending)) {
            if ($shown -ge 6) {
                Write-Dim ("... и ещё {0}" -f ($inGroup.Count - $shown))
                break
            }
            $rel = $row.Path.Substring($root.Length).TrimStart("\")
            Write-Dim ("{0,9:N1} МБ  {1}" -f ($row.Size / 1MB), $rel)
            $shown++
        }
    }
}

if (-not $Apply) {
    Write-Nzl "Это пробный прогон. Ничего не изменено."
    Write-Nzl "Убрать в корзину:  .\NZL.bat clean -Apply"
    Write-Nzl "Удалить сразу:     .\NZL.bat clean -Apply -Delete"
    exit 0
}
if ($rows.Count -eq 0) { exit 0 }

$moved = 0
$failed = 0
foreach ($row in $rows) {
    try {
        $isOldTrash = $row.Name -like "_trash-*"
        if ($Delete -or $isOldTrash) {
            Remove-Item -LiteralPath $row.Path -Recurse -Force -ErrorAction Stop
        } else {
            $rel = $row.Path.Substring($root.Length).TrimStart("\")
            $dest = Join-Path $trash $rel
            $destDir = Split-Path $dest -Parent
            if (-not (Test-Path -LiteralPath $destDir)) {
                New-Item -ItemType Directory -Path $destDir -Force | Out-Null
            }
            Move-Item -LiteralPath $row.Path -Destination $dest -Force -ErrorAction Stop
        }
        $moved++
    } catch {
        $failed++
        Write-Nzl ("не удалось: {0} - {1}" -f $row.Name, $_.Exception.Message)
    }
}

if ($Delete) {
    Write-Nzl ("Удалено {0} из {1} объектов." -f $moved, $rows.Count)
} else {
    Write-Nzl ("Убрано в корзину {0} из {1} объектов." -f $moved, $rows.Count)
    Write-Nzl ("Корзина: {0}" -f $trash)
    Write-Nzl "Проверил - удали папку корзины целиком."
}
if ($failed -gt 0) {
    Write-Nzl "Часть не удалась: закрой Visual Studio, игру и оверлей и повтори."
}

Write-Nzl "В рабочей папке осталось:"
$keep = Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue |
        Sort-Object -Property @{ Expression = { -not $_.PSIsContainer } }, Name
foreach ($item in $keep) {
    $suffix = ""
    if ($item.PSIsContainer) { $suffix = "\" }
    Write-Dim ($item.Name + $suffix)
}
Write-Nzl "Где что искать: docs\LIVE-RUN.md - порядок запуска, HANDOFF.md - состояние работы."
