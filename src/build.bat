@echo off
setlocal
rem Build the DXGI proxy -> build\dxgi.dll (single self-contained file).
rem install.py copies it next to acs.exe.
rem
rem Exports are asm jmp thunks that jmp through pointers resolved at load time from
rem the real System32 dxgi.dll (real_ptrs.c) — no renamed dxgi_real.dll on disk.
rem Thunks (not .def forwarders) because MSVC only emits forwarders for a pure
rem /NOENTRY DLL with no object file, and we need DllMain. See gen_def.py.

rem override these via environment if your setup differs
if not defined VCVARS set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not defined PY set PY=python
set HERE=%~dp0
set OUT=%HERE%build

if not exist "%VCVARS%" ( echo ERROR: vcvars64 not found at %VCVARS% - set VCVARS to your VS install & exit /b 1 )
call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvars64 failed & exit /b 1 )
if not exist "%OUT%" mkdir "%OUT%"

echo [1/3] generating thunks.asm / exports.def / real_ptrs.c / dxgi_names.h
rem %HERE% ends in a backslash, which would escape the closing quote — hence the dot.
"%PY%" "%HERE%gen_def.py" "%HERE%." >nul
if errorlevel 1 ( echo ERROR: gen_def.py failed & exit /b 1 )

echo [2/3] assembling thunks
ml64 /nologo /c /Fo"%OUT%\thunks.obj" "%HERE%thunks.asm" >nul
if errorlevel 1 ( echo ERROR: ml64 failed & exit /b 1 )

rem Offline investigation modules (frame capture, NGX/CSP observation) are not part of
rem the shipped mod and live in the research repo. Point ACRE_RESEARCH_DIR at that
rem folder to build them in; otherwise no-op stubs are compiled so the call sites link.
set RESEARCH_SRC="%HERE%research_stubs.cpp"
set RESEARCH_DEF=
if defined ACRE_RESEARCH_DIR if exist "%ACRE_RESEARCH_DIR%\cap.cpp" (
  set RESEARCH_SRC="%ACRE_RESEARCH_DIR%\cap.cpp" "%ACRE_RESEARCH_DIR%\ngx_spy.cpp"
  set RESEARCH_DEF=/DACRE_RESEARCH
  echo     research build: including cap.cpp + ngx_spy.cpp from %ACRE_RESEARCH_DIR%
)

echo [3/3] compiling + linking dxgi.dll
rem Every "%HERE%" / "%OUT%\" needs a trailing '.' or filename: a backslash before the
rem closing quote escapes it, and cl then swallows the rest of the command line.
set NGX=%HERE%..\vendor\DLSS
set MH=%HERE%..\vendor\minhook
rem MinHook (C, no CRT security warnings) built alongside our sources.
cl /nologo /LD /W4 /O2 /MT %RESEARCH_DEF% /D_CRT_SECURE_NO_WARNINGS /I"%HERE%." /I"%NGX%\include" /I"%MH%\include" ^
   /Fo%OUT%\ /Fe"%OUT%\dxgi.dll" ^
   "%HERE%dxgi_proxy.c" "%HERE%real_ptrs.c" "%HERE%dxgi_hook.cpp" "%HERE%ngx_dlss.cpp" "%HERE%dlss_pass.cpp" "%HERE%om_hook.cpp" "%HERE%submit_hook.cpp" "%HERE%res_hook.cpp" "%HERE%config.cpp" "%HERE%diag.cpp" %RESEARCH_SRC% ^
   "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
   "%OUT%\thunks.obj" ^
   /link /DEF:"%HERE%exports.def" ^
   "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" ^
   d3d11.lib dxgi.lib user32.lib advapi32.lib shell32.lib ole32.lib ^
   /OUT:"%OUT%\dxgi.dll"
if errorlevel 1 ( echo ERROR: compile/link failed & exit /b 1 )

echo.
echo === exports ===
dumpbin /nologo /exports "%OUT%\dxgi.dll" | findstr /R /C:"^ *[0-9]"
echo.
echo built %OUT%\dxgi.dll
endlocal
