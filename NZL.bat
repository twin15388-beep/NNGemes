@echo off
rem ===========================================================================
rem  NZL Studio - dispatcher. Works from ANY directory, including the
rem  C:\Windows\system32 that a fresh elevated PowerShell starts in.
rem
rem  Usage, PowerShell, from the project folder:
rem    cd "D:\NZL Rob run"
rem    .\NZL.bat overlay      ESP (INSERT = menu, END = exit)
rem    .\NZL.bat probe        transport + chain, game must be in a match
rem    .\NZL.bat report       full report, zipped, copied to Dropbox
rem    .\NZL.bat build        driver + apps + host tests
rem    .\NZL.bat load         map the driver, before starting the game
rem    .\NZL.bat crash        BSOD dumps and bugcheck codes
rem    .\NZL.bat clean        clean the working folder (dry run by default)
rem  From any folder: PowerShell needs '& "D:\NZL Rob run\NZL.bat" overlay',
rem  cmd.exe is happy with the bare quoted path.
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
    set "RC=%ERRORLEVEL%"
    if not "%RC%"=="0" pause
    exit /b %RC%
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %2 %3 %4 %5 %6
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo [NZL] exit %RC% - read the message above.
    pause
)
exit /b %RC%

:unknown
echo [NZL] No such tool: %TOOL%
echo [NZL] Expected file: %PS1%
echo [NZL] Available:
for %%F in ("%ROOT%tools\NZL-*.ps1") do echo    %%~nF
pause
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
echo   NZL.bat clean     clean the working folder (dry run; -Apply to delete)
echo.
echo   PowerShell from the project folder (no operator needed):
echo     .\NZL.bat overlay
echo   PowerShell with a full path (the call operator IS needed):
echo     ^& "%ROOT%NZL.bat" overlay
echo   cmd.exe with a full path (quotes are enough):
echo     "%ROOT%NZL.bat" overlay
echo.
rem Double-clicked with no tool: keep the window open so the list is readable.
pause
exit /b 1
