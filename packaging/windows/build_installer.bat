@echo off
rem Build a native RapidEditor and make one NSIS installer for this machine.
rem Usage: build_installer.bat [version]  (default 1.0.0)
rem Architecture is detected from PROCESSOR_ARCHITECTURE; override with
rem   set RE_ARCH=x64   or   set RE_ARCH=arm64
rem This file is intentionally ASCII-only so it works with any cmd code page.
setlocal
cd /d "%~dp0\..\.."

set "VER=%~1"
if "%VER%"=="" set "VER=1.0.0"

if /i "%PROCESSOR_ARCHITECTURE%"=="x86" (
  echo ERROR: 32-bit Windows is not supported.
  exit /b 1
)
set "ARCH=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=arm64"
if not "%RE_ARCH%"=="" set "ARCH=%RE_ARCH%"

echo [1/3] Configuring Release build (%ARCH%)...
cmake -S . -B "build-win-%ARCH%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail
echo [2/3] Building RapidEditor.exe...
cmake --build "build-win-%ARCH%" --config Release
if errorlevel 1 goto :fail

where makensis >nul 2>nul
if errorlevel 1 (
  echo ERROR: makensis not found. Install NSIS ^(https://nsis.sourceforge.io^) or use CI.
  goto :fail
)

if not exist "dist" mkdir "dist"
if not exist "dist\payload" mkdir "dist\payload"
copy /y "build-win-%ARCH%\RapidEditor.exe" "dist\payload\RapidEditor-%ARCH%.exe" >nul
if errorlevel 1 goto :fail

echo [3/3] Creating NSIS installer...
set "OUT=%CD%\dist\RapidEditor-%VER%-windows-%ARCH%-setup.exe"
if /i "%ARCH%"=="arm64" (
  makensis "-DPRODUCT_VERSION=%VER%" "-DOUTFILE=%OUT%" "-DPAYLOAD_ARM64=%CD%\dist\payload\RapidEditor-arm64.exe" packaging\windows\installer.nsi
) else (
  makensis "-DPRODUCT_VERSION=%VER%" "-DOUTFILE=%OUT%" "-DPAYLOAD_X64=%CD%\dist\payload\RapidEditor-x64.exe" packaging\windows\installer.nsi
)
if errorlevel 1 goto :fail

echo Done: %OUT%
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
