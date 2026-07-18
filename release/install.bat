@echo off
setlocal enabledelayedexpansion
title AC DLSS/DLAA installer

set "HERE=%~dp0"
set "AC_DIR="

rem --- 1. drag-and-drop: acs.exe or the AC folder dropped onto this script ---
if not "%~1"=="" (
    if /i "%~nx1"=="acs.exe" (
        set "AC_DIR=%~dp1"
    ) else if exist "%~1\acs.exe" (
        set "AC_DIR=%~1\"
    )
)

rem --- 2. registry: default Steam install ---
if not defined AC_DIR (
    for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Valve\Steam" /v InstallPath 2^>nul') do set "STEAM=%%B"
    if defined STEAM if exist "!STEAM!\steamapps\common\assettocorsa\acs.exe" set "AC_DIR=!STEAM!\steamapps\common\assettocorsa\"
)

rem --- 3. common secondary-library locations ---
if not defined AC_DIR (
    for %%D in (
        "C:\Program Files (x86)\Steam"
        "C:\SteamLibrary" "D:\SteamLibrary" "E:\SteamLibrary" "F:\SteamLibrary"
        "D:\Steam" "E:\Steam"
    ) do (
        if not defined AC_DIR if exist "%%~D\steamapps\common\assettocorsa\acs.exe" (
            set "AC_DIR=%%~D\steamapps\common\assettocorsa\"
        )
    )
)

rem --- 4. confirm an auto-detected path, or fall through to manual entry ---
if defined AC_DIR (
    echo Found Assetto Corsa at:
    echo   !AC_DIR!
    set /p "CONFIRM=Install here? [Y/n] "
    if /i "!CONFIRM!"=="n" set "AC_DIR="
)

:ask_manual
if not defined AC_DIR (
    echo.
    echo Couldn't find Assetto Corsa automatically.
    echo Either re-run this installer by dragging your acs.exe onto it, or paste
    echo the full path to your Assetto Corsa folder below ^(the one with acs.exe
    echo in it^) and press Enter:
    set "AC_DIR="
    set /p "AC_DIR=> "
    set "AC_DIR=!AC_DIR:"=!"
)
if not exist "!AC_DIR!\acs.exe" (
    echo.
    echo No acs.exe found in "!AC_DIR!" - try again.
    set "AC_DIR="
    goto ask_manual
)

rem --- 5. refuse while the game is running: the DLL would be locked ---
tasklist /FI "IMAGENAME eq acs.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "acs.exe" >nul
if not errorlevel 1 (
    echo.
    echo Assetto Corsa is running - close it first, then run this installer again.
    pause
    exit /b 1
)

rem --- 6. install ---
copy /y "%HERE%dxgi.dll" "!AC_DIR!dxgi.dll" >nul
if not exist "!AC_DIR!acre.ini" (
    copy /y "%HERE%acre.ini" "!AC_DIR!acre.ini" >nul
    echo installed acre.ini ^(default: mode=dlaa^)
) else (
    echo acre.ini already present - left as-is
)
rem dxgi_real.dll lets the proxy forward to the real DXGI - generated from THIS
rem machine's own System32 copy, never shipped in the release.
copy /y "%WINDIR%\System32\dxgi.dll" "!AC_DIR!dxgi_real.dll" >nul
if exist "!AC_DIR!acre_proxy.log" del "!AC_DIR!acre_proxy.log"

echo.
echo Installed to !AC_DIR!
echo Launch Assetto Corsa in VR as usual. Settings live in acre.ini there -
echo edit and save it while the game is running to apply changes live.
echo.
pause
exit /b 0
