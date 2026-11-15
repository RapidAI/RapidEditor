@echo off
rem CI helper: build the x64-only, arm64-only and universal NSIS installers
rem from already-built payloads. All output goes to dist\.
rem
rem   make_installers.bat <version> <x64-exe> <arm64-exe>
rem
rem Either payload may be "-" if it is unavailable; at least one is required.
setlocal
cd /d "%~dp0\..\.."

set "VER=%~1"
set "X64=%~2"
set "ARM64=%~3"
if "%VER%"=="" set "VER=1.0.0"

where makensis >nul 2>nul
if errorlevel 1 (
  echo ERROR: makensis not found in PATH.
  exit /b 1
)
if not exist "dist" mkdir "dist"

set "HAVE_X64=0"
set "HAVE_ARM64=0"
if not "%X64%"=="" if not "%X64%"=="-" if exist "%X64%" set "HAVE_X64=1"
if not "%ARM64%"=="" if not "%ARM64%"=="-" if exist "%ARM64%" set "HAVE_ARM64=1"
if "%HAVE_X64%%HAVE_ARM64%"=="00" (
  echo ERROR: no payload found. Expected two paths to RapidEditor.exe.
  exit /b 1
)

if "%HAVE_X64%"=="1" (
  echo Building x64 installer...
  makensis "-DPRODUCT_VERSION=%VER%" "-DOUTFILE=%CD%\dist\RapidEditor-%VER%-windows-x64-setup.exe" "-DPAYLOAD_X64=%X64%" packaging\windows\installer.nsi
  if errorlevel 1 exit /b 1
)
if "%HAVE_ARM64%"=="1" (
  echo Building arm64 installer...
  makensis "-DPRODUCT_VERSION=%VER%" "-DOUTFILE=%CD%\dist\RapidEditor-%VER%-windows-arm64-setup.exe" "-DPAYLOAD_ARM64=%ARM64%" packaging\windows\installer.nsi
  if errorlevel 1 exit /b 1
)
if "%HAVE_X64%%HAVE_ARM64%"=="11" (
  echo Building universal installer...
  makensis "-DPRODUCT_VERSION=%VER%" "-DOUTFILE=%CD%\dist\RapidEditor-%VER%-windows-universal-setup.exe" "-DPAYLOAD_X64=%X64%" "-DPAYLOAD_ARM64=%ARM64%" packaging\windows\installer.nsi
  if errorlevel 1 exit /b 1
)

echo Done. Contents of dist:
dir /b dist\*.exe
exit /b 0
