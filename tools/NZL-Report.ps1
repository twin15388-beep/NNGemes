<#
NZL Studio - one-shot state report (Windows PowerShell).
Read-only: does NOT load the driver, does NOT start the game, does NOT run any BAT.

Why: the arena agent cannot see D:\NZL Rob run. Every round trip costs a day when
the log that answers the question is sitting on disk unread. This script packs the
build result, the CTest result, the probe output, the crash info and the file hashes
into one text file (plus an optional zip), so one command closes the loop.

Usage (from the repo root, no admin needed for the report itself):
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\NZL-Report.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\NZL-Report.ps1 -GameDir "D:\tools\steamapps\common\Rob Run"
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\NZL-Report.ps1 -Build -Zip -CopyToDropbox

Output:
  nzl_report_<yyyyMMdd-HHmmss>.txt  in the repo root
  nzl_report_<yyyyMMdd-HHmmss>.zip  with -Zip
  copy in %USERPROFILE%\Dropbox\   with -CopyToDropbox (only if that folder exists)

Nothing here is destructive: it only reads files, writes the report, and optionally
copies the report into Dropbox.
#>
param(
    [string]$GameDir = "",
    [switch]$Build,
    [switch]$Zip,
    [switch]$CopyToDropbox
)

try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
$ErrorActionPreference = 'Continue'

function Write-Nzl([string]$m) { Write-Host "[NZL] $m" -ForegroundColor Magenta }

$root = Split-Path $PSScriptRoot -Parent
if (-not $root) { $root = (Get-Location).Path }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$report = Join-Path $root "nzl_report_$stamp.txt"
$global:NzlReport = $report

"" | Set-Content -Path $report -Encoding UTF8

function Add-Line([string]$text) {
    $text | Add-Content -Path $global:NzlReport -Encoding UTF8
}

function Add-Section([string]$title) {
    Add-Line ""
    Add-Line ("=" * 78)
    Add-Line "== $title"
    Add-Line ("=" * 78)
}

function Add-File([string]$path, [string]$title, [int]$tailLines = 0) {
    Add-Section $title
    Add-Line "path: $path"
    if (-not (Test-Path $path)) { Add-Line "MISSING"; return }
    $item = Get-Item $path
    Add-Line "size: $($item.Length)  modified: $($item.LastWriteTime.ToString('o'))"
    try {
        $hash = (Get-FileHash -Path $path -Algorithm SHA256).Hash
        Add-Line "sha256: $hash"
    } catch { Add-Line "sha256: FAILED $($_.Exception.Message)" }
    if ($item.Length -gt 0) {
        try {
            if ($tailLines -gt 0) {
                Get-Content $path -Tail $tailLines -ErrorAction Stop |
                    ForEach-Object { Add-Line $_ }
            } else {
                Get-Content $path -ErrorAction Stop | ForEach-Object { Add-Line $_ }
            }
        } catch { Add-Line "read failed: $($_.Exception.Message)" }
    }
}

function Invoke-Native([string]$exe, [string[]]$arguments) {
    try {
        $out = & $exe @arguments 2>&1
        if ($null -eq $out) { return "<no output>" }
        return ($out | Out-String).TrimEnd()
    } catch { return "FAILED: $($_.Exception.Message)" }
}

# ---------------------------------------------------------------- header
Add-Section "NZL report"
Add-Line "generated: $((Get-Date).ToString('o'))"
Add-Line "root: $root"
Add-Line "host: $($env:COMPUTERNAME)  user: $($env:USERNAME)"
Add-Line "powershell: $($PSVersionTable.PSVersion)"
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Add-Line "os: $($os.Caption) $($os.Version) build $($os.BuildNumber)"
} catch { Add-Line "os: unknown ($($_.Exception.Message))" }
Add-Line "admin: $((New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"

# ---------------------------------------------------------------- git
Add-Section "git"
Add-Line "branch: $(Invoke-Native 'git' @('-C', $root, 'rev-parse', '--abbrev-ref', 'HEAD'))"
Add-Line "head:   $(Invoke-Native 'git' @('-C', $root, 'rev-parse', 'HEAD'))"
Add-Line ""
Add-Line "--- git log -5 ---"
Add-Line (Invoke-Native 'git' @('-C', $root, 'log', '-5', '--oneline'))
Add-Line ""
Add-Line "--- git status --short ---"
Add-Line (Invoke-Native 'git' @('-C', $root, 'status', '--short'))
Add-Line ""
Add-Line "--- shared wire header present (proof the pull landed) ---"
foreach ($f in @('nzl\include\nzl\na_wire.h', 'nzl\include\nzl\na_scan.h', 'nzl\tests\test_protocol.cpp')) {
    $p = Join-Path $root $f
    Add-Line ("{0,-40} {1}" -f $f, (Test-Path $p))
}

# ---------------------------------------------------------------- toolchain
Add-Section "toolchain"
Add-Line "cmake: $(Invoke-Native 'cmake' @('--version'))"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    Add-Line "vswhere: $(Invoke-Native $vswhere @('-latest', '-products', '*', '-property', 'installationVersion'))"
    Add-Line "msvc dir: $(Invoke-Native $vswhere @('-latest', '-products', '*', '-property', 'installationPath'))"
} else {
    Add-Line "vswhere: MISSING"
}
$kitsRoot = ""
foreach ($rkPath in @("HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots",
                      "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots")) {
    if ($kitsRoot) { break }
    try {
        $rk = Get-ItemProperty -Path $rkPath -Name KitsRoot10 -ErrorAction Stop
        if ($rk.KitsRoot10) { $kitsRoot = $rk.KitsRoot10 }
    } catch { }
}
if (-not $kitsRoot) {
    foreach ($candidate in @("C:\Program Files (x86)\Windows Kits\10", "D:\Windows Kits\10", "C:\Program Files\Windows Kits\10")) {
        if (-not $kitsRoot -and (Test-Path (Join-Path $candidate "Include"))) { $kitsRoot = $candidate }
    }
}
Add-Line "winsdk root: $(if ($kitsRoot) { $kitsRoot } else { 'NOT FOUND' })"
$kits = if ($kitsRoot) { Join-Path $kitsRoot "Include" } else { "" }
if ($kits -and (Test-Path $kits)) {
    Add-Line "winsdk: $((Get-ChildItem $kits -Directory | Select-Object -ExpandProperty Name) -join ', ')"
    $km = Get-ChildItem $kits -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'km\ntddk.h') }
    if ($km) { Add-Line "wdk km headers: $($km.Name -join ', ')" } else { Add-Line "wdk km headers: MISSING" }
} else { Add-Line "winsdk: MISSING at $kits" }

# ---------------------------------------------------------------- build
if ($Build) {
    Add-Section "NZL-BuildAll.ps1 (captured output)"
    $scriptPath = Join-Path $root "tools\NZL-BuildAll.ps1"
    $buildLogPath = Join-Path $root "build_all.log"
    Add-Line "running: $scriptPath"
    $buildStart = Get-Date
    $buildExit = -1
    try {
        $captured = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath 2>&1
        $buildExit = $LASTEXITCODE
    } catch {
        $captured = @("BuildAll failed to start: $($_.Exception.Message)")
    }
    $captured | ForEach-Object { Add-Line "$_" }
    try { $captured | Set-Content -Path $buildLogPath -Encoding UTF8 } catch {}
    Add-Line ""
    Add-Line "BuildAll exit=$buildExit  duration=$([int]((Get-Date) - $buildStart).TotalSeconds)s"
    Add-Line "full transcript also written to: $buildLogPath"
    Write-Nzl "BuildAll exit=$buildExit"
}

# ---------------------------------------------------------------- driver
Add-File (Join-Path $root "driver_build.log") "driver build log (full)" 0
foreach ($candidate in @("nzl\driver\driver.sys", "driver.sys")) {
    $p = Join-Path $root $candidate
    if (Test-Path $p) {
        Add-Section "driver.sys"
        $i = Get-Item $p
        Add-Line "path: $p"
        Add-Line "size: $($i.Length)  modified: $($i.LastWriteTime.ToString('o'))"
        Add-Line "sha256: $((Get-FileHash $p -Algorithm SHA256).Hash)"
        $src = Get-Item (Join-Path $root "nzl\driver\sc_drv.c") -ErrorAction SilentlyContinue
        if ($src) {
            Add-Line "sc_drv.c modified: $($src.LastWriteTime.ToString('o'))"
            if ($i.LastWriteTime -lt $src.LastWriteTime) {
                Add-Line "VERDICT: STALE - driver.sys is older than sc_drv.c, rebuild with NZL-BuildAll.ps1."
            } else {
                Add-Line "VERDICT: driver.sys is newer than sc_drv.c."
            }
        }
        break
    }
}

# ---------------------------------------------------------------- build dir
$build = Join-Path $root "build-nzl"
Add-Section "build-nzl"
if (Test-Path $build) {
    Add-Line "exists: yes"
    $cache = Join-Path $build "CMakeCache.txt"
    if (Test-Path $cache) {
        Get-Content $cache | Where-Object {
            $_ -match 'CMAKE_CXX_COMPILER:|CMAKE_GENERATOR:|CMAKE_BUILD_TYPE:'
        } | ForEach-Object { Add-Line $_ }
    }
    $log = Join-Path $build "Testing\Temporary\LastTest.log"
    Add-Line ""
    Add-Line "--- LastTest.log ---"
    if (Test-Path $log) {
        $lt = Get-Item $log
        Add-Line "modified: $($lt.LastWriteTime.ToString('o'))  size: $($lt.Length)"
        Get-Content $log | ForEach-Object { Add-Line $_ }
    } else {
        Add-Line "MISSING (ctest did not run, or configure failed before tests)"
    }
    Add-Line ""
    Add-Line "--- built executables ---"
    $exes = Get-ChildItem $build -Recurse -Include *.exe -ErrorAction SilentlyContinue |
        Sort-Object Name
    if ($exes) {
        foreach ($e in $exes) {
            Add-Line ("{0,-22} {1,10}  {2}" -f $e.Name, $e.Length, $e.LastWriteTime.ToString('o'))
        }
    } else {
        Add-Line "no .exe found under build-nzl"
    }
} else {
    Add-Line "MISSING - build never configured"
}

# ---------------------------------------------------------------- probe
Add-File (Join-Path $root "nzl_probe.txt") "nzl_probe.txt" 0

# ---------------------------------------------------------------- crashes
Add-Section "crash info"
try {
    $ev = Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001} -MaxEvents 5 -ErrorAction Stop
    foreach ($e in $ev) { Add-Line "BugCheck $($e.TimeCreated): $($e.Message -replace "`r?`n", ' ')" }
} catch { Add-Line "BugCheck events: none/failed ($($_.Exception.Message))" }
if (Test-Path "C:\Windows\Minidump") {
    Get-ChildItem "C:\Windows\Minidump\*.dmp" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 5 | ForEach-Object {
            Add-Line ("dump: {0}  {1}  {2}" -f $_.Name, $_.Length, $_.LastWriteTime.ToString('o'))
        }
} else { Add-Line "C:\Windows\Minidump: missing" }

# ---------------------------------------------------------------- package
Add-Section "package artefacts"
Get-ChildItem $root -Filter "NZL-Studio*.bat" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 5 | ForEach-Object {
        Add-Line ("{0,-52} {1,10}  {2}" -f $_.Name, $_.Length, $_.LastWriteTime.ToString('o'))
    }
foreach ($z in @("NZL-Studio-Package.zip")) {
    $p = Join-Path $root $z
    if (Test-Path $p) {
        Add-Line ("{0,-52} {1,10}  {2}" -f $z, (Get-Item $p).Length, (Get-Item $p).LastWriteTime.ToString('o'))
    }
}

# ---------------------------------------------------------------- game files
Add-Section "game fingerprint"
if (-not $GameDir) {
    $guess = @(
        "D:\tools\steamapps\common\Rob Run",
        "C:\Program Files (x86)\Steam\steamapps\common\Rob Run"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($guess) { $GameDir = $guess }
}
if ($GameDir -and (Test-Path $GameDir)) {
    Add-Line "game dir: $GameDir"
    $metaPath = Join-Path $GameDir "global-metadata.dat"
    if (-not (Test-Path $metaPath)) {
        $hit = Get-ChildItem $GameDir -Recurse -Filter "global-metadata.dat" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) { $metaPath = $hit.FullName }
    }
    $gameFiles = @(
        @{ name = "GameAssembly.dll";    path = (Join-Path $GameDir "GameAssembly.dll") },
        @{ name = "UnityPlayer.dll";     path = (Join-Path $GameDir "UnityPlayer.dll") },
        @{ name = "ROB RUN.exe";         path = (Join-Path $GameDir "ROB RUN.exe") },
        @{ name = "global-metadata.dat"; path = $metaPath }
    )
    foreach ($entry in $gameFiles) {
        $f = $entry.name
        $p = $entry.path
        if ($p -and (Test-Path $p)) {
            $i = Get-Item $p
            Add-Line ("{0,-24} size={1,12}  {2}" -f $f, $i.Length, $i.LastWriteTime.ToString('o'))
            Add-Line ("  path   {0}" -f $p)
            Add-Line ("  sha256 {0}" -f (Get-FileHash $p -Algorithm SHA256).Hash)
        } else {
            Add-Line ("{0,-24} MISSING" -f $f)
        }
    }
    Add-Line "eac folder: $(Test-Path (Join-Path $GameDir 'EasyAntiCheat'))"
} else {
    Add-Line "game dir: not found (pass -GameDir '...')"
}

# ---------------------------------------------------------------- verdict
Add-Section "verdict"
$head = Invoke-Native 'git' @('-C', $root, 'rev-parse', '--short', 'HEAD')
Add-Line "commit under test: $head"
$log = Join-Path $build "Testing\Temporary\LastTest.log"
if (Test-Path $log) {
    $content = Get-Content $log -Raw
    foreach ($suite in @('nzl_tests', 'nzl_pipeline_tests', 'nzl_profile_tests', 'nzl_skeleton_tests', 'nzl_protocol_tests')) {
        Add-Line ("{0,-22} ran={1}" -f $suite, ($content -match [regex]::Escape($suite)))
    }
    if ($content -match '(\d+)% tests passed, (\d+) tests failed out of (\d+)') {
        Add-Line "ctest summary: $($Matches[1])% passed, $($Matches[2]) failed, $($Matches[3]) total"
    }
}
foreach ($exe in @("nzl_protocol_tests.exe", "nzl_probe.exe", "nzl_overlay.exe")) {
    $p = Join-Path $build ("Release\" + $exe)
    if (-not (Test-Path $p)) { $p = Join-Path $build $exe }
    Add-Line ("{0,-24} built={1}" -f $exe, (Test-Path $p))
}
$listsMtime = (Get-Item (Join-Path $root "nzl\CMakeLists.txt") -ErrorAction SilentlyContinue).LastWriteTime
$cacheMtime = (Get-Item (Join-Path $build "CMakeCache.txt") -ErrorAction SilentlyContinue).LastWriteTime
Add-Line "nzl/CMakeLists.txt modified: $listsMtime"
Add-Line "build-nzl/CMakeCache.txt modified: $cacheMtime"
if ($listsMtime -and $cacheMtime) {
    if ($cacheMtime -lt $listsMtime) {
        Add-Line "VERDICT: CONFIGURE IS STALE - build-nzl predates nzl/CMakeLists.txt, the new test target is not in the project."
    } else {
        Add-Line "VERDICT: configure is newer than nzl/CMakeLists.txt."
    }
}
Add-Line "nzl_probe.txt: $(Test-Path (Join-Path $root 'nzl_probe.txt'))"
Add-Line "NOTE: report only. No driver was loaded, no BAT and no game was started."

Write-Nzl "report: $report"

if ($Zip) {
    $zipPath = [System.IO.Path]::ChangeExtension($report, ".zip")
    try {
        Compress-Archive -Path $report -DestinationPath $zipPath -Force
        Write-Nzl "zip: $zipPath"
    } catch { Write-Nzl "zip failed: $($_.Exception.Message)" }
}

if ($CopyToDropbox) {
    $dbx = Join-Path $env:USERPROFILE "Dropbox"
    if (Test-Path $dbx) {
        try {
            Copy-Item $report $dbx -Force
            Write-Nzl "copied to $dbx"
        } catch { Write-Nzl "copy to Dropbox failed: $($_.Exception.Message)" }
    } else {
        Write-Nzl "no %USERPROFILE%\Dropbox folder - paste the txt instead"
    }
}

Write-Nzl "send the report back: $report"
exit 0
