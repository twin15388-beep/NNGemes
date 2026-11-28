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
. (Join-Path $PSScriptRoot "nzl-dumpheader.ps1")
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
$buildDir = Join-Path $root "build-nzl"
Add-Section "build-nzl"
if (Test-Path $buildDir) {
    Add-Line "exists: yes"
    $cache = Join-Path $buildDir "CMakeCache.txt"
    if (Test-Path $cache) {
        Get-Content $cache | Where-Object {
            $_ -match 'CMAKE_CXX_COMPILER:|CMAKE_GENERATOR:|CMAKE_BUILD_TYPE:'
        } | ForEach-Object { Add-Line $_ }
    }
    $log = Join-Path $buildDir "Testing\Temporary\LastTest.log"
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
    $exes = Get-ChildItem $buildDir -Recurse -Include *.exe -ErrorAction SilentlyContinue |
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
Add-Section "nzl_probe.txt"
$probeCandidates = @()
foreach ($rel in @("build-nzl\Release\nzl_probe.txt", "build-nzl\nzl_probe.txt", "nzl_probe.txt")) {
    $full = Join-Path $root $rel
    if (Test-Path $full) { $probeCandidates += Get-Item $full }
}
if ($probeCandidates.Count -eq 0) {
    Add-Line "MISSING - the probe has not been run from build-nzl\Release"
} else {
    $probe = $probeCandidates | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    Add-Line "path: $($probe.FullName)"
    Add-Line "size: $($probe.Length)  modified: $($probe.LastWriteTime.ToString('o'))"
    Add-Line "sha256: $((Get-FileHash $probe.FullName -Algorithm SHA256).Hash)"
    Add-Line "other copies found: $($probeCandidates.Count - 1)"
    try { Get-Content $probe.FullName -ErrorAction Stop | ForEach-Object { Add-Line $_ } }
    catch { Add-Line "read failed: $($_.Exception.Message)" }
    $global:NzlProbeInfo = $probe
}

# ---------------------------------------------------------------- crashes
Add-Section "crash info"
try {
    $ev = Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001} -MaxEvents 5 -ErrorAction Stop
    foreach ($e in $ev) {
        Add-Line "BugCheck $($e.TimeCreated): $($e.Message -replace "`r?`n", ' ')"
        # The message is localised ("Код ошибки: 0x0000001e (...)"), so pull the
        # numbers out by shape instead of trusting the words.
        $hex = [regex]::Matches($e.Message, '0x[0-9a-fA-F]{8,16}')
        if ($hex.Count -gt 0) {
            Add-Line ("  parsed: code={0}  args={1}" -f $hex[0].Value, (($hex | Select-Object -Skip 1 | ForEach-Object { $_.Value }) -join ' '))
        }
    }
} catch { Add-Line "BugCheck events: none/failed ($($_.Exception.Message))" }
# The stop code is in the first 0x60 bytes of the dump. Decode it here so the
# numbers survive a localised event log and do not depend on WinDbg.
if (Test-Path "C:\Windows\Minidump") {
    $dumps = Get-ChildItem "C:\Windows\Minidump\*.dmp" -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending
    if (-not $dumps) { Add-Line "C:\Windows\Minidump: empty" }
    foreach ($d in ($dumps | Select-Object -First 3)) {
        Add-Line ("dump: {0}  {1}  {2}" -f $d.Name, $d.Length, $d.LastWriteTime.ToString('o'))
        $hdr = Read-NzlDumpHeader $d.FullName
        foreach ($l in ((Format-NzlDumpHeader $hdr) -split "`r?`n")) { if ($l) { Add-Line $l } }
    }
} else { Add-Line "C:\Windows\Minidump: missing" }
$bootTime = $null
try { $bootTime = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime } catch {}
if ($bootTime) { Add-Line "last boot: $($bootTime.ToString('o'))" }
if (Test-Path "C:\Windows\MEMORY.DMP") {
    $m = Get-Item "C:\Windows\MEMORY.DMP"
    Add-Line ("MEMORY.DMP: {0} bytes  {1}" -f $m.Length, $m.LastWriteTime.ToString('o'))
    foreach ($l in ((Format-NzlDumpHeader (Read-NzlDumpHeader $m.FullName)) -split "`r?`n")) { if ($l) { Add-Line $l } }
    if ($bootTime -and $m.LastWriteTime -lt $bootTime) {
        Add-Line "VERDICT: MEMORY.DMP is older than the last boot - it belongs to a PREVIOUS crash, not this one."
    } else {
        Add-Line "VERDICT: MEMORY.DMP is from this boot session - it is the crash we are chasing."
    }
} else { Add-Line "MEMORY.DMP: missing (kernel dump not written - CrashDumpEnabled may not have taken effect)" }

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
$log = Join-Path $buildDir "Testing\Temporary\LastTest.log"
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
    $p = Join-Path $buildDir ("Release\" + $exe)
    if (-not (Test-Path $p)) { $p = Join-Path $buildDir $exe }
    Add-Line ("{0,-24} built={1}" -f $exe, (Test-Path $p))
}
# CMakeCache.txt is written once at the first configure and is NOT touched by
# later reconfigures, so its mtime proves nothing. The generated project file
# is regenerated every time: that is the real evidence that the new target
# exists in build-nzl.
$protocolProject = Join-Path $buildDir "nzl_protocol_tests.vcxproj"
Add-Line "nzl/CMakeLists.txt modified: $((Get-Item (Join-Path $root "nzl\CMakeLists.txt") -ErrorAction SilentlyContinue).LastWriteTime)"
Add-Line "generated nzl_protocol_tests.vcxproj: $(Test-Path $protocolProject)"
if (Test-Path $protocolProject) {
    Add-Line "  vcxproj modified: $((Get-Item $protocolProject).LastWriteTime.ToString('o'))"
    Add-Line "VERDICT: configure generated the protocol test target."
} else {
    Add-Line "VERDICT: nzl_protocol_tests.vcxproj is MISSING - configure did not pick up nzl/CMakeLists.txt."
}
if ($global:NzlProbeInfo) {
    Add-Line "nzl_probe.txt: $($global:NzlProbeInfo.LastWriteTime.ToString('o'))"
    $driverInfo = Get-Item (Join-Path $root "nzl\driver\driver.sys") -ErrorAction SilentlyContinue
    if ($driverInfo -and $global:NzlProbeInfo.LastWriteTime -lt $driverInfo.LastWriteTime) {
        Add-Line "VERDICT: probe log PREDATES driver.sys - it is an old run, not a test of this driver."
    }
} else {
    Add-Line "nzl_probe.txt: NOT FOUND"
}
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

Add-Section "EOF"
Add-Line "complete: yes"
Add-Line "if this marker is missing the report was cut short by a runtime error - send it anyway"

Write-Nzl "send the report back: $report"
exit 0
