@echo off
rem ===========================================================================
rem  NZL Studio - dispatcher. Works from ANY directory, including the
rem  C:\Windows\system32 that a fresh elevated PowerShell starts in.
rem
rem  Usage (full path, so the current directory does not matter):
rem    "D:\NZL Rob run\NZL.bat" overlay
rem    "D:\NZL Rob run\NZL.bat" probe
rem    "D:\NZL Rob run\NZL.bat" report
rem    "D:\NZL Rob run\NZL.bat" build
rem    "D:\NZL Rob run\NZL.bat" load
rem    "D:\NZL Rob run\NZL.bat" crash
rem
rem  Tools: overlay probe report build load crash dump package collect analyze
rem  Everything runs as tools\NZL-<name>.ps1 next to this file. Extra
rem  arguments are passed through: NZL.bat report -NoZip
rem ===========================================================================
setlocal
set "ROOT=%~dp0"
set "TOOL=%~1"

if "%TOOL%"=="" goto usage
if /i "%TOOL%"=="help" goto usage
if /i "%TOOL%"=="?" goto usage

set "PS1=%ROOT%tools\NZL-%TOOL%.ps1"
if not exist "%PS1%" goto unknown

rem The scripts locate the project through $PSScriptRoot, so the caller's
rem working directory is irrelevant - but cd anyway so relative paths that
rem the scripts print (nzl_overlay.txt, crash_info\) land in the project.
cd /d "%ROOT%"

if /i "%TOOL%"=="report" if "%2"=="" (
    echo [NZL] report with -Zip -CopyToDropbox
    powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Zip -CopyToDropbox
    exit /b %ERRORLEVEL%
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %2 %3 %4 %5 %6
exit /b %ERRORLEVEL%

:unknown
echo [NZL] No such tool: %TOOL%
echo [NZL] Expected file: %PS1%
echo [NZL] Available:
for %%F in ("%ROOT%tools\NZL-*.ps1") do echo    %%~nF
exit /b 2

:usage
echo NZL Studio dispatcher
echo.
echo   NZL.bat overlay   ESP overlay        (INSERT = menu, END = exit)
echo   NZL.bat probe     transport + chain  (game must be in a match)
echo   NZL.bat report    full report, zipped and copied to Dropbox
echo   NZL.bat build     driver + apps + host tests
echo   NZL.bat load      map the driver     (before starting the game)
echo   NZL.bat crash     collect BSOD dumps and bugcheck codes
echo   NZL.bat dump      switch to kernel memory dumps
echo.
echo   Full path works from any directory:
echo     "%ROOT%NZL.bat" overlay
exit /b 1
