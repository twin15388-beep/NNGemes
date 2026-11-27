<#
NZL Studio - скачать Il2CppDumper (Perfare) с GitHub в C:\Il2CppDumper.
Запуск:
  powershell -ExecutionPolicy Bypass -File tools\NZL-GetDumper.ps1
Потом:
  powershell -ExecutionPolicy Bypass -File tools\NZL-Dump.ps1 -DumperDir "C:\Il2CppDumper"
#>
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }
$dst = "C:\Il2CppDumper"
# A modern .NET build (net6/net7/net8) leaves Il2CppDumper.runtimeconfig.json
# next to the exe and dies with "You must install or update .NET" on machines
# without that runtime (observed live). The net472 build runs on the .NET
# Framework 4.8 shipped with Windows 11 - prefer it, replace a net6 copy.
if ((Test-Path (Join-Path $dst "Il2CppDumper.exe")) -and
    -not (Test-Path (Join-Path $dst "Il2CppDumper.runtimeconfig.json"))) {
    Write-Nzl "Il2CppDumper уже на месте: $dst\Il2CppDumper.exe"; exit 0
}
if (Test-Path (Join-Path $dst "Il2CppDumper.runtimeconfig.json")) {
    Write-Nzl "Найдена .NET (net6+) сборка дампера - заменяю на net472, чтобы не ставить runtime."
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/Perfare/Il2CppDumper/releases/latest" -Headers @{ "User-Agent" = "NZL-Studio" }
} catch { Write-Nzl "FAIL: GitHub API недоступен: $($_.Exception.Message). Скачай вручную: https://github.com/Perfare/Il2CppDumper/releases"; exit 2 }
Write-Nzl "Релиз: $($rel.tag_name)"
$asset = $rel.assets | Where-Object { $_.name -like "*net472*win*.zip" } | Select-Object -First 1
if (-not $asset) { $asset = $rel.assets | Where-Object { $_.name -like "*win*.zip" -and $_.name -notlike "*net6*" -and $_.name -notlike "*net7*" -and $_.name -notlike "*net8*" } | Select-Object -First 1 }
if (-not $asset) { $asset = $rel.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1 }
if (-not $asset) { Write-Nzl "FAIL: zip-ассет не найден в релизе. Скачай вручную с GitHub releases."; exit 3 }
Write-Nzl "Качаю: $($asset.name)"
$zip = "C:\Il2CppDumper.zip"
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -Headers @{ "User-Agent" = "NZL-Studio" }
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Expand-Archive -Path $zip -DestinationPath $dst -Force
# exe may be nested in a subfolder - flatten
$exe = Get-ChildItem $dst -Filter "Il2CppDumper.exe" -Recurse | Select-Object -First 1
if ($exe -and $exe.DirectoryName -ne $dst) { Copy-Item (Join-Path $exe.DirectoryName "*") $dst -Recurse -Force }
if (Test-Path (Join-Path $dst "Il2CppDumper.exe")) {
    Write-Nzl "Il2CppDumper OK: $dst\Il2CppDumper.exe"
    Write-Nzl "Теперь: powershell -ExecutionPolicy Bypass -File tools\NZL-Dump.ps1 -DumperDir `"C:\Il2CppDumper`""
    exit 0
}
Write-Nzl "FAIL: Il2CppDumper.exe после распаковки не найден в $dst. Загляни в папку и пришли dir."
exit 4
