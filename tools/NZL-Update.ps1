<#
NZL Studio - обновление рабочей папки из репозитория.

Локальные правки в .bat/.ps1 (обычно это испорченные переводы строк после
старых сборок) блокируют git pull: "Your local changes would be overwritten".
Скрипт откладывает такие файлы в сторону, а не удаляет: копия остаётся в
_trash-<дата>\mine\.

  .\NZL.bat update            показать, что будет обновлено
  .\NZL.bat update -Apply     обновить
#>
param(
    [switch]$Apply
)

try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
function Write-Dim([string]$m) { Write-Host "      $m" -ForegroundColor DarkGray }

$root = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root

if (-not (Test-Path -LiteralPath (Join-Path $root ".git"))) {
    Write-Nzl "FAIL: это не git-копия проекта, обновлять нечего."
    exit 2
}
$branch = (& git rev-parse --abbrev-ref HEAD 2>&1)
if ($LASTEXITCODE -ne 0) {
    Write-Nzl "FAIL: git не отвечает. Установлен ли Git в PATH?"
    exit 3
}
Write-Nzl "Ветка: $branch"
$head = (& git rev-parse --short HEAD 2>&1)
Write-Nzl "Сейчас: $head"

# Файлы под git, которые локально изменены. Обычно это .bat с \r\r\n.
$dirty = @(& git status --porcelain 2>&1 |
           Where-Object { $_ -match "^.{2}\s" } |
           Where-Object { $_ -match "\.(bat|ps1)$" -or $_ -match "\.ps1$" })
if ($dirty.Count -gt 0) {
    Write-Nzl "Локально изменённые файлы, которые мешают pull:"
    foreach ($line in $dirty) { Write-Dim $line.Trim() }
} else {
    Write-Nzl "Мешающих локальных правок нет."
}

if (-not $Apply) {
    Write-Nzl "Пробный прогон. Обновить: .\NZL.bat update -Apply"
    exit 0
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$keepDir = Join-Path $root ("_trash-" + $stamp + "\mine")
foreach ($line in $dirty) {
    $relative = $line.Substring(3).Trim()
    if (-not $relative) { continue }
    $source = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $source)) { continue }
    if (-not (Test-Path -LiteralPath $keepDir)) {
        New-Item -ItemType Directory -Path $keepDir -Force | Out-Null
    }
    $target = Join-Path $keepDir ($relative -replace "[\\/]", "_")
    Copy-Item -LiteralPath $source -Destination $target -Force -ErrorAction SilentlyContinue
    Write-Nzl "Сохранил копию: $target"
    & git checkout -- $relative 2>&1 | Out-Null
}

& git pull --ff-only 2>&1 | ForEach-Object { Write-Dim $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Nzl "FAIL: pull не прошёл. Пришли вывод выше."
    exit 4
}
$newHead = (& git rev-parse --short HEAD 2>&1)
Write-Nzl "Готово: $head -> $newHead"
Write-Nzl "Дальше: .\NZL.bat build, перезагрузка, .\NZL.bat load, игра, .\NZL.bat overlay"
