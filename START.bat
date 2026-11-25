@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
for /f %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"
title by govno leak
pushd "%~dp0"
if errorlevel 1 goto folder_error
if not exist "RobRun.exe" goto missing_exe
if not exist "robrun.py" goto missing_py

set "CHOICE=%LOCALAPPDATA%\robrun\visit.txt"
set "VISIT=0"
if exist "%CHOICE%" goto saved_choice
call :banner
echo     Хотите ли вы посетить страницы авторов?
echo     [Y] да    [N] нет
call :ask_yn
if "%errorlevel%"=="1" set "VISIT=1"
echo.
echo     Сохранить выбор на будущее?
echo     [Y] да    [N] нет
call :ask_yn
if "%errorlevel%"=="1" goto save_choice
goto fresh_screen

:save_choice
if not exist "%LOCALAPPDATA%\robrun" mkdir "%LOCALAPPDATA%\robrun"
if "%VISIT%"=="1" (echo yes>"%CHOICE%") else (echo no>"%CHOICE%")
goto fresh_screen

:saved_choice
set "SAVED="
set /p SAVED=<"%CHOICE%"
if /i "%SAVED%"=="yes" set "VISIT=1"

:fresh_screen
cls
call :banner
if not "%VISIT%"=="1" goto pages_done
start "" "https://yougame.biz/cash/"
start "" "https://yougame.biz/work/"
start "" "https://t.me/fakeanul"

:pages_done
set "ROBRUN_PY_EXE="
set "ROBRUN_PY_ARGS="
call :find_python
if defined ROBRUN_PY_EXE goto start

echo Python 3.10+ не найден. Ставлю Python 3.13 через winget...
where winget >nul 2>&1
if errorlevel 1 goto missing_winget
call winget install --id Python.Python.3.13 --exact --source winget --scope user --silent --disable-interactivity --accept-package-agreements --accept-source-agreements
if errorlevel 1 goto install_error
call :find_python
if not defined ROBRUN_PY_EXE goto python_not_found

:start
call "%ROBRUN_PY_EXE%" %ROBRUN_PY_ARGS% -u "robrun.py"
set "EC=%errorlevel%"
if not "%EC%"=="0" ( echo. & echo Ошибка, код %EC% )
goto hold

:banner
echo.
echo %ESC%[93m    $$$  $  $       $$$  $$$ $  $ $  $  $$$      $    $$$   $$  $  $ 
echo %ESC%[93m    $  $  $$       $    $  $ $  $ $$ $ $  $      $    $    $  $ $ $  
echo %ESC%[93m    $$$    $       $ $$ $  $ $  $ $ $$ $  $      $    $$$  $$$$ $$   
echo %ESC%[93m    $  $   $       $  $ $  $  $$  $  $ $  $      $    $    $  $ $ $  
echo %ESC%[93m    $$$    $        $$$  $$$  $$  $  $  $$$      $$$  $$$  $  $ $  $ 
echo.
echo %ESC%[33m    $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
echo %ESC%[93m    $                         by govno leak                         $
echo %ESC%[33m    $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
echo.
echo %ESC%[0m
exit /b 0

:find_python
call python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1
if not errorlevel 1 ( set "ROBRUN_PY_EXE=python" & exit /b 0 )
call py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1
if not errorlevel 1 ( set "ROBRUN_PY_EXE=py" & set "ROBRUN_PY_ARGS=-3" & exit /b 0 )
for %%P in (
    "%LOCALAPPDATA%\Programs\Python\Python314\python.exe"
    "%LOCALAPPDATA%\Programs\Python\Python313\python.exe"
    "%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
) do (
    if exist "%%~P" (
        call "%%~P" -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1
        if not errorlevel 1 ( set "ROBRUN_PY_EXE=%%~P" & exit /b 0 )
    )
)
exit /b 1

:missing_exe
echo Нет RobRun.exe в этой папке.
goto failure
:missing_py
echo Нет robrun.py
goto failure
:missing_winget
echo Установи Python с python.org и запусти снова.
goto failure
:install_error
echo winget не смог поставить Python.
goto failure
:python_not_found
echo Python не найден после установки. Перезапусти батник.
goto failure
:failure
goto hold
:folder_error
goto hold

:ask_yn
where powershell >nul 2>&1
if errorlevel 1 (
    choice /c YN /n >nul
    if errorlevel 2 (echo     нет & exit /b 0)
    echo     да
    exit /b 1
)
powershell -NoProfile -Command "while($true){$k=[Console]::ReadKey($true);if($k.Key -eq 'Y' -or $k.KeyChar -eq [char]0x0414 -or $k.KeyChar -eq [char]0x0434){exit 1};if($k.Key -eq 'N' -or $k.KeyChar -eq [char]0x0422 -or $k.KeyChar -eq [char]0x0442){exit 0}}"
if "%errorlevel%"=="1" (echo     да & exit /b 1)
echo     нет
exit /b 0

:hold
ping -n 86400 127.0.0.1 >nul
goto hold
