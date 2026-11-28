<#
NZL Studio - включить Kernel memory dump (Windows, админ) перед загрузкой
драйвера. Triage-дамп прошлого BSOD дал нулевой стек, поэтому ставим
CrashDumpEnabled=2 (kernel dump -> C:\Windows\MEMORY.DMP + minidump).
Перезагрузка требуется только если эти настройки изменяются; повторный запуск
при уже настроенном dump не перезагружает ПК и не загружает драйвер.
  powershell -ExecutionPolicy Bypass -File tools\NZL-SetKernelDump.ps1
Откат на маленькие дампы: CrashDumpEnabled=3.
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Nzl "FAIL: нужен администратор."; exit 2 }
$key = "HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl"
$settings = Get-ItemProperty $key -ErrorAction SilentlyContinue
$cur = $settings.CrashDumpEnabled
$dumpFile = [string]$settings.DumpFile
$miniDir = [string]$settings.MinidumpDir
Write-Nzl "Текущий CrashDumpEnabled: $cur (1=complete, 2=kernel, 3=small, 7=auto)"
$needsReboot = ($cur -ne 2) -or ($dumpFile -ne "C:\Windows\MEMORY.DMP") -or ($miniDir -ne "C:\Windows\Minidump")
if (-not $needsReboot) {
    Write-Nzl "Kernel dump уже настроен. Перезагрузка не нужна; не запускай этот setup при каждом старте."
    exit 0
}
Set-ItemProperty -Path $key -Name CrashDumpEnabled -Value 2 -Type DWord
Set-ItemProperty -Path $key -Name DumpFile -Value "C:\Windows\MEMORY.DMP" -Type ExpandString
Set-ItemProperty -Path $key -Name MinidumpDir -Value "C:\Windows\Minidump" -Type ExpandString
Write-Nzl "Kernel dump настройки изменены. Перезагрузи ПК один раз, чтобы применить их; при повторном запуске без изменений reboot не понадобится."
