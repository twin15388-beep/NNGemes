<#
NZL Studio - сбор диагностики после BSOD (Windows, PowerShell от администратора).
Драйвер НЕ загружает, игру НЕ запускает. Только читает журналы и копирует minidump.
Использование:
  powershell -ExecutionPolicy Bypass -File tools\NZL-CrashInfo.ps1
Результат: папка crash_info\ (crash_info.txt + последний .dmp). Отправь мне оба.
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot "nzl-dumpheader.ps1")
$out = Join-Path $root "crash_info"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = Join-Path $out "crash_info.txt"
"NZL crash info $(Get-Date -Format o)" | Set-Content $log -Encoding UTF8

function Add-Log([string]$s) { $s | Add-Content $log -Encoding UTF8; Write-Nzl $s }

# --- Событие BugCheck (стоп-код и файл дампа) ---
try {
    $ev = Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001} -MaxEvents 3 -ErrorAction Stop
    foreach ($e in $ev) { Add-Log "BugCheck event $($e.TimeCreated): $($e.Message -replace "`r?`n", ' ')" }
} catch { Add-Log "BugCheck event 1001: not found ($($_.Exception.Message))" }
try {
    $ev41 = Get-WinEvent -FilterHashtable @{LogName='System'; Id=41} -MaxEvents 3 -ErrorAction Stop
    foreach ($e in $ev41) { Add-Log "Kernel-Power 41 $($e.TimeCreated)" }
} catch {}

# --- Minidump ---
$dumpDir = "C:\Windows\Minidump"
if (Test-Path $dumpDir) {
    $dumps = Get-ChildItem $dumpDir -Filter "*.dmp" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
    if ($dumps) {
        $d = $dumps[0]
        Add-Log "Latest minidump: $($d.FullName) size=$($d.Length) time=$($d.LastWriteTime)"
        try {
            Copy-Item $d.FullName (Join-Path $out $d.Name) -Force
            Add-Log "Copied to crash_info\$($d.Name) - send me this file."
        } catch { Add-Log "Copy failed (need admin): $($_.Exception.Message)" }
        $dumps | Select-Object -First 3 | ForEach-Object {
            Add-Log "  dump: $($_.Name) $($_.LastWriteTime)"
            foreach ($l in ((Format-NzlDumpHeader (Read-NzlDumpHeader $_.FullName)) -split "`r?`n")) { if ($l) { Add-Log $l } }
        }
    } else { Add-Log "Minidump folder empty - crash dumps may be disabled." }
} else { Add-Log "C:\Windows\Minidump not found." }
if (Test-Path "C:\Windows\MEMORY.DMP") {
    $m = Get-Item "C:\Windows\MEMORY.DMP"
    Add-Log "MEMORY.DMP exists size=$($m.Length) time=$($m.LastWriteTime) (do NOT send whole, minidump is enough)"
    foreach ($l in ((Format-NzlDumpHeader (Read-NzlDumpHeader $m.FullName)) -split "`r?`n")) { if ($l) { Add-Log $l } }
}
Add-Log "NOTE: after reboot the kdmapper driver is GONE (manual map). Do not load driver.sys again until I say so."
Write-Nzl "Send me: crash_info\crash_info.txt + the .dmp file from crash_info\"
