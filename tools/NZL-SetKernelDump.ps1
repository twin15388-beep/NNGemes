<#
NZL Studio - включить Kernel memory dump (Windows, админ) ПЕРЕД повторной
загрузкой драйвера. Triage-дамп прошлого BSOD дал нулевой стек, поэтому
ставим CrashDumpEnabled=2 (kernel dump -> C:\Windows\MEMORY.DMP + minidump).
Требуется перезагрузка после применения. Драйвер она НЕ загружает.
  powershell -ExecutionPolicy Bypass -File tools\NZL-SetKernelDump.ps1
Откат на маленькие дампы: CrashDumpEnabled=3.
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Nzl "FAIL: нужен администратор."; exit 2 }
$key = "HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl"
$cur = (Get-ItemProperty $key -ErrorAction SilentlyContinue).CrashDumpEnabled
Write-Nzl "Текущий CrashDumpEnabled: $cur (1=complete, 2=kernel, 3=small, 7=auto)"
Set-ItemProperty -Path $key -Name CrashDumpEnabled -Value 2 -Type DWord
Set-ItemProperty -Path $key -Name DumpFile -Value "C:\Windows\MEMORY.DMP" -Type ExpandString -ErrorAction SilentlyContinue
Set-ItemProperty -Path $key -Name MinidumpDir -Value "C:\Windows\Minidump" -Type ExpandString -ErrorAction SilentlyContinue
Write-Nzl "Установлен Kernel dump. ПЕРЕЗАГРУЗИ ПК сейчас (драйвер после этого грузим один раз, по шагам)."
