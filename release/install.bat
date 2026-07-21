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

rem --- 4. confirm an auto-detected path, or fall through to manual entry.
rem (skip the prompt on an elevated relaunch - the user already confirmed) ---
if defined AC_DIR if /i not "%~2"=="__elevated__" (
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
tasklist /FI "IMAGENAME eq acs.exe" 2>nul | find /i "acs.exe" >nul
if not errorlevel 1 (
    echo.
    echo Assetto Corsa is running - close it first, then run this installer again.
    pause
    exit /b 1
)

rem --- 6. elevate ONLY if the AC folder isn't writable (Steam's default under
rem Program Files needs admin; a library folder on another drive usually doesn't).
rem Probe with a throwaway file so we don't pop a UAC prompt when it's unnecessary. ---
set "WTEST=!AC_DIR!__acre_write_test.tmp"
( echo test ) > "!WTEST!" 2>nul
if exist "!WTEST!" (
    del "!WTEST!" >nul 2>&1
) else (
    net session >nul 2>&1
    if errorlevel 1 (
        echo.
        echo "!AC_DIR!" needs administrator rights to write into - requesting...
        rem Re-launch elevated, handing the resolved acs.exe path back as arg 1 so
        rem the elevated run skips straight to the install with no re-prompting.
        powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0' -ArgumentList @('!AC_DIR!acs.exe','__elevated__')" 2>nul
        if errorlevel 1 (
            echo.
            echo Could not elevate automatically. Right-click install.bat and pick
            echo "Run as administrator", then try again.
            pause
        )
        exit /b
    )
    rem Already elevated but still can't write - a real permissions/AV problem.
    goto copy_fail
)

rem --- 7. install (every copy is checked; a failure aborts loudly instead of
rem     pretending to succeed) ---
copy /y "%HERE%dxgi.dll" "!AC_DIR!dxgi.dll" >nul
if errorlevel 1 goto copy_fail
if not exist "!AC_DIR!acre.ini" (
    copy /y "%HERE%acre.ini" "!AC_DIR!acre.ini" >nul
    if errorlevel 1 goto copy_fail
    echo installed acre.ini ^(default: mode=dlaa^)
) else (
    echo acre.ini already present - left as-is
)
rem AC/CSP usually already ships this; only fill the gap, never overwrite a
rem working copy that's already there.
if exist "%HERE%nvngx_dlss.dll" (
    if not exist "!AC_DIR!nvngx_dlss.dll" (
        copy /y "%HERE%nvngx_dlss.dll" "!AC_DIR!nvngx_dlss.dll" >nul
        if errorlevel 1 goto copy_fail
        echo installed nvngx_dlss.dll ^(NVIDIA DLSS runtime^)
    ) else (
        echo nvngx_dlss.dll already present - left as-is
    )
)
rem dxgi_real.dll lets the proxy forward to the real DXGI - generated from THIS
rem machine's own System32 copy, never shipped in the release. Without it AC
rem fails at launch, so verify it landed. (The proxy imports this by name, so the
rem loader fills the forward table before any of our code runs - which is exactly
rem why forwarding must go through a renamed file, not a runtime LoadLibrary.)
copy /y "%WINDIR%\System32\dxgi.dll" "!AC_DIR!dxgi_real.dll" >nul
if errorlevel 1 goto copy_fail
if not exist "!AC_DIR!dxgi_real.dll" goto copy_fail
if exist "!AC_DIR!acre_proxy.log" del "!AC_DIR!acre_proxy.log"

echo.
echo Installed to !AC_DIR!
echo Launch Assetto Corsa in VR as usual. Settings live in acre.ini there -
echo edit and save it while the game is running to apply changes live.
echo.
pause
exit /b 0

:copy_fail
echo.
echo *** INSTALL FAILED: a file could not be copied into
echo     "!AC_DIR!"
echo.
echo Most likely the folder is write-protected. Make sure Assetto Corsa is
echo fully closed, then run this installer again ^(it should have already asked
echo for administrator rights^). If it still fails, your antivirus may be
echo blocking the copy.
echo.
pause
exit /b 1
