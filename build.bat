@echo off
rem ===========================================================================
rem  selflog build script - MSVC (Visual Studio 2022) x64 console app
rem  output: dist\selflog.exe   (/MT static CRT -> single self-contained file)
rem
rem  Keep this file ASCII-only: cmd.exe parses .bat with the OEM code page.
rem
rem  Toolchain discovery is done explicitly (no vcvars64.bat) because vcvars
rem  shells out to reg.exe, which some locked-down / sandboxed environments
rem  block. If explicit discovery fails we still fall back to vcvars64.bat.
rem ===========================================================================
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "BUILD=%ROOT%build"
set "DIST=%ROOT%dist"

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%DIST%"  mkdir "%DIST%"

rem --- locate Visual Studio ------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build] vswhere.exe not found. Install Visual Studio 2022 with the C++ workload.
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [build] No Visual Studio installation with the C++ toolset found.
    exit /b 1
)

rem --- locate MSVC toolset (newest first) ----------------------------------
set "VCTOOLS="
for /f "delims=" %%d in ('dir /b /ad /o-n "%VSPATH%\VC\Tools\MSVC" 2^>nul') do if not defined VCTOOLS set "VCTOOLS=%VSPATH%\VC\Tools\MSVC\%%d"

rem --- locate Windows 10/11 SDK (newest first) -----------------------------
set "SDKROOT=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKVER="
if exist "%SDKROOT%\Include" (
    for /f "delims=" %%d in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKVER set "SDKVER=%%d"
)

if defined VCTOOLS if defined SDKVER (
    set "PATH=%VCTOOLS%\bin\Hostx64\x64;%SDKROOT%\bin\%SDKVER%\x64;%PATH%"
    set "INCLUDE=%VCTOOLS%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\winrt"
    set "LIB=%VCTOOLS%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"
    echo [build] MSVC: %VCTOOLS%
    echo [build] SDK : %SDKVER%
) else (
    echo [build] explicit discovery failed, falling back to vcvars64.bat
    call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
)

where cl.exe >nul 2>nul || ( echo [build] cl.exe not found in PATH & exit /b 1 )
where rc.exe >nul 2>nul || ( echo [build] rc.exe not found in PATH & exit /b 1 )

rem --- resources (version info) --------------------------------------------
rc /nologo /fo "%BUILD%\selflog.res" "%SRC%\selflog.rc"
if errorlevel 1 goto fail

rem --- compile + link -------------------------------------------------------
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE "%SRC%\selflog.c" "%BUILD%\selflog.res" /Fo:"%BUILD%\selflog.obj" /Fe:"%DIST%\selflog.exe" /link /SUBSYSTEM:CONSOLE /INCREMENTAL:NO advapi32.lib
if errorlevel 1 goto fail

echo.
echo [build] OK -^> %DIST%\selflog.exe
for %%f in ("%DIST%\selflog.exe") do echo [build] size: %%~zf bytes
exit /b 0

:fail
echo.
echo [build] FAILED
exit /b 1
