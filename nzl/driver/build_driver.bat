@echo off
setlocal enabledelayedexpansion
rem Build the kernel driver with MSVC and WDK headers/libraries.
rem Output: x64\Release\driver.sys, copied to driver.sys in this folder and to
rem ..\1_run\driver.sys and ..\1_run\kernel-mode.sys (the image kd2 maps).
rem No Visual Studio WDK integration is required.
cd /d "%~dp0"

rem Locate Windows Kits root: registry KitsRoot10 first, then known roots.
set "WINSDK="
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul') do set "WINSDK=%%B"
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul') do if not defined WINSDK set "WINSDK=%%B"
if defined WINSDK if "%WINSDK:~-1%"=="\" set "WINSDK=%WINSDK:~0,-1%"
if not defined WINSDK set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
if not exist "%WINSDK%\Include" set "WINSDK=C:\Program Files\Windows Kits\10"
set "SDK="
for /d %%D in ("%WINSDK%\Include\10.*") do (
    if exist "%%D\km\ntddk.h" set "SDK=%%~nxD"
)
if not defined SDK (
    echo [ERROR] No WDK kernel headers [km\ntddk.h] under "%WINSDK%\Include".
    echo         Kits found:
    dir /b "%WINSDK%\Include" 2^>nul
    echo         Run tools\NZL-FindWdk.ps1 and send its output.
    exit /b 1
)
echo Using Windows Kits "%WINSDK%" SDK %SDK%

rem Find Visual Studio via vswhere (any drive/edition), fallback to known paths.
set "VS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%I"
)
if not defined VS (
    for %%P in (
        "C:\Program Files\Microsoft Visual Studio\18\Community"
        "C:\Program Files\Microsoft Visual Studio\18\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Community"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
        "D:\programs\vs"
        "D:\Program Files\Microsoft Visual Studio\2022\Community"
        "E:\Program Files\Microsoft Visual Studio\2022\Community"
    ) do (
        if not defined VS if exist "%%~P\VC\Tools\MSVC" set "VS=%%~P"
    )
)
if not defined VS (
    echo [ERROR] Visual Studio with the C++ toolset not found.
    exit /b 1
)

set "MSVC="
for /d %%D in ("%VS%\VC\Tools\MSVC\*") do set "MSVC=%%~nxD"
if not defined MSVC (
    echo [ERROR] no MSVC toolset under "%VS%\VC\Tools\MSVC"
    exit /b 1
)

set "CLX=%VS%\VC\Tools\MSVC\%MSVC%\bin\Hostx64\x64\cl.exe"
set "LNKX=%VS%\VC\Tools\MSVC\%MSVC%\bin\Hostx64\x64\link.exe"
if not exist "%CLX%" (
    echo [ERROR] cl.exe not found: "%CLX%"
    exit /b 1
)
rem NOTE: never name a variable LINK / CL / PATH here - link.exe reads the LINK
rem environment variable as extra command line and "C:\Program Files" splits it.
set "INC_KM=%WINSDK%\Include\%SDK%\km"
set "INC_SH=%WINSDK%\Include\%SDK%\shared"
set "INC_CRT=%WINSDK%\Include\%SDK%\km\crt"
set "LIB_KM=%WINSDK%\Lib\%SDK%\km\x64"

if not exist "%INC_KM%\ntddk.h" (
    echo [ERROR] WDK headers not found under "%INC_KM%"
    echo         Install "Windows Driver Kit" for SDK %SDK%.
    exit /b 1
)

if not exist "x64\Release" mkdir "x64\Release"

echo [1/2] compiling sc_drv.c  (MSVC %MSVC%, SDK %SDK%)
"%CLX%" /c /nologo /W3 /O2 /D_WIN64 /D_AMD64_ /DAMD64 /DNDEBUG /MT /GS- /Zc:wchar_t /Zc:forScope /GR- ^
  /I"%INC_KM%" /I"%INC_SH%" /I"%INC_CRT%" /kernel ^
  /Fo"x64\Release\sc_drv.obj" sc_drv.c
if errorlevel 1 ( echo [FAILED] compile & exit /b 1 )

echo [2/2] linking driver.sys
"%LNKX%" /OUT:"x64\Release\driver.sys" /NOLOGO ^
  /NODEFAULTLIB ^
  /LIBPATH:"%LIB_KM%" ^
  ntoskrnl.lib hal.lib wdmsec.lib ^
  /SUBSYSTEM:NATIVE /DRIVER /ENTRY:DriverEntry /MACHINE:X64 /KERNEL ^
  "x64\Release\sc_drv.obj"
if errorlevel 1 ( echo [FAILED] link & exit /b 1 )

copy /Y "x64\Release\driver.sys" "driver.sys" >nul
if errorlevel 1 ( echo [FAILED] copy & exit /b 1 )
echo.
echo BUILD_OK  -^>  x64\Release\driver.sys  (copied to driver.sys in this folder)
echo.
echo Load with kdmapper as admin, then run NZL Studio (it reads HKLM\SYSTEM\NaCoreFilter). 
exit /b 0
