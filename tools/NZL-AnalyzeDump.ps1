<#
NZL Studio - анализ minidump после BSOD (Windows). Ищет cdb.exe из WDK
(Debugging Tools for Windows) и гоняет !analyze -v по последнему дампу.
Драйвер НЕ загружает. Использование (PowerShell, обычный):
  powershell -ExecutionPolicy Bypass -File tools\NZL-AnalyzeDump.ps1
  powershell -ExecutionPolicy Bypass -File tools\NZL-AnalyzeDump.ps1 -Dump "D:\NZL Rob run\crash_info\112726-11109-01.dmp"
Результат: crash_info\analyze.txt - пришли мне его целиком.
#>
param([string]$Dump = "")
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent

# --- cdb.exe ---
$cdb = $null
foreach ($c in @(
    "D:\Windows Kits\10\Debuggers\x64\cdb.exe",
    "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
    "C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe"
)) { if (Test-Path $c) { $cdb = $c; break } }
if (-not $cdb) {
    $found = Get-ChildItem "D:\Windows Kits", "C:\Program Files (x86)\Windows Kits" -Filter "cdb.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $cdb = $found.FullName }
}
if (-not $cdb) {
    Write-Nzl "cdb.exe не найден. Поставь Debugging Tools: winget install Microsoft.WinDbg, или WDK компонент Debuggers."
    Write-Nzl "Альтернатива вручную: открой дамп в WinDbg, выполни !analyze -v и пришли текст."
    exit 2
}
Write-Nzl "cdb: $cdb"

# --- дамп ---
if ($Dump -eq "") {
    # Kernel dump first: triage minidump gave ZEROED_STACK last time.
    if (Test-Path "C:\Windows\MEMORY.DMP") { $Dump = "C:\Windows\MEMORY.DMP" }
    else {
        $cand = Get-ChildItem (Join-Path $root "crash_info") -Filter "*.dmp" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($cand) { $Dump = $cand.FullName }
    }
}
if ($Dump -eq "" -or -not (Test-Path $Dump)) {
    $cand = Get-ChildItem "C:\Windows\Minidump" -Filter "*.dmp" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($cand) { $Dump = $cand.FullName }
}
if ($Dump -eq "" -or -not (Test-Path $Dump)) { Write-Nzl "FAIL: .dmp не найден. Сначала tools\NZL-CrashInfo.ps1"; exit 3 }
Write-Nzl "dump: $Dump"

$env:_NT_SYMBOL_PATH = "srv*C:\symbols*https://msdl.microsoft.com/download/symbols"
$outDir = Join-Path $root "crash_info"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir "analyze.txt"
Write-Nzl "Анализирую (нужен интернет для символов Microsoft)..."
& $cdb -z $Dump -c "!analyze -v; .ecxr; kv; lm n; q" 2>&1 | Tee-Object -FilePath $out
Write-Nzl "Готово. Пришли мне файл: $out"
