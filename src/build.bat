@echo off
setlocal
rem Build the DXGI proxy -> build\dxgi.dll + build\dxgi_real.dll (copy of system DXGI).
rem install.py copies both next to acs.exe.
rem
rem Exports are asm jmp thunks rather than .def forwarders: MSVC only emits forwarders
rem for a pure /NOENTRY DLL with no object file, and we need DllMain. See gen_def.py.

rem override these via environment if your setup differs
if not defined VCVARS set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not defined PY set PY=python
set HERE=%~dp0
set OUT=%HERE%build

if not exist "%VCVARS%" ( echo ERROR: vcvars64 not found at %VCVARS% - set VCVARS to your VS install & exit /b 1 )
call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvars64 failed & exit /b 1 )
if not exist "%OUT%" mkdir "%OUT%"

echo [1/4] generating thunks.asm / exports.def / dxgi_names.h
rem %HERE% ends in a backslash, which would escape the closing quote — hence the dot.
"%PY%" "%HERE%gen_def.py" "%HERE%." >nul
if errorlevel 1 ( echo ERROR: gen_def.py failed & exit /b 1 )

echo [2/4] copying system dxgi.dll -^> dxgi_real.dll
copy /Y "C:\Windows\System32\dxgi.dll" "%OUT%\dxgi_real.dll" >nul
if errorlevel 1 ( echo ERROR: could not copy system dxgi.dll & exit /b 1 )

echo [3/5] building dxgi_real.lib (import library)
lib /nologo /DEF:"%HERE%dxgi_real.def" /OUT:"%OUT%\dxgi_real.lib" /MACHINE:X64 >nul
if errorlevel 1 ( echo ERROR: lib failed & exit /b 1 )

echo [4/5] assembling thunks
ml64 /nologo /c /Fo"%OUT%\thunks.obj" "%HERE%thunks.asm" >nul
if errorlevel 1 ( echo ERROR: ml64 failed & exit /b 1 )

echo [5/5] compiling + linking dxgi.dll
rem Every "%HERE%" / "%OUT%\" needs a trailing '.' or filename: a backslash before the
rem closing quote escapes it, and cl then swallows the rest of the command line.
set NGX=%HERE%..\vendor\DLSS
set MH=%HERE%..\vendor\minhook
rem MinHook (C, no CRT security warnings) built alongside our sources.
cl /nologo /LD /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /I"%HERE%." /I"%NGX%\include" /I"%MH%\include" ^
   /Fo%OUT%\ /Fe"%OUT%\dxgi.dll" ^
   "%HERE%dxgi_proxy.c" "%HERE%dxgi_hook.cpp" "%HERE%ngx_dlss.cpp" "%HERE%dlss_pass.cpp" "%HERE%om_hook.cpp" "%HERE%submit_hook.cpp" "%HERE%res_hook.cpp" "%HERE%config.cpp" "%HERE%diag.cpp" "%HERE%cap.cpp" ^
   "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
   "%OUT%\thunks.obj" ^
   /link /DEF:"%HERE%exports.def" "%OUT%\dxgi_real.lib" ^
   "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" ^
   d3d11.lib dxgi.lib user32.lib advapi32.lib shell32.lib ole32.lib ^
   /OUT:"%OUT%\dxgi.dll"
if errorlevel 1 ( echo ERROR: compile/link failed & exit /b 1 )

echo.
echo === exports ===
dumpbin /nologo /exports "%OUT%\dxgi.dll" | findstr /R /C:"^ *[0-9]"
echo.
echo built %OUT%\dxgi.dll and %OUT%\dxgi_real.dll
endlocal
