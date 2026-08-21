@echo off
setlocal EnableExtensions

set "CONFIG=%~1"
if not defined CONFIG set "CONFIG=Release"

set "VALID_CONFIG="
for %%C in (Debug Release RelWithDebInfo MinSizeRel) do (
    if /I "%CONFIG%"=="%%C" set "VALID_CONFIG=1"
)
if not defined VALID_CONFIG (
    echo Invalid configuration: %CONFIG%
    echo Usage: build.cmd [Debug^|Release^|RelWithDebInfo^|MinSizeRel]
    exit /b 2
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer's vswhere.exe was not found.
    echo Install Visual Studio with the Desktop development with C++ workload.
    exit /b 1
)

set "VS_INSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_INSTALL=%%I"
)
if not defined VS_INSTALL (
    echo No Visual Studio installation with the x64 C++ toolchain was found.
    exit /b 1
)

set "VCVARS=%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo MSVC environment script was not found: "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%

where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo cmake.exe was not found in PATH.
    exit /b 1
)
where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo ninja.exe was not found in PATH.
    echo Install Ninja or the Visual Studio C++ CMake tools component.
    exit /b 1
)

rem Appending a dot avoids a trailing backslash immediately before a quote.
set "SOURCE_DIR=%~dp0."
set "BUILD_DIR=%~dp0build-local"

cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "Ninja Multi-Config"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel
exit /b %errorlevel%
