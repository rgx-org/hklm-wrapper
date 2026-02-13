@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "CMAKE_WRAPPER=%SCRIPT_DIR%cmake-msvc-x86.cmd"
if not exist "%CMAKE_WRAPPER%" (
  echo [ERROR] Missing helper script: "%CMAKE_WRAPPER%".
  exit /b 1
)

echo [INFO] Configuring preset windows-x86-msvc-release-stage ...
call "%CMAKE_WRAPPER%" --preset windows-x86-msvc-release-stage
if errorlevel 1 exit /b %errorlevel%

echo [INFO] Building preset windows-x86-msvc-release-stage ...
call "%CMAKE_WRAPPER%" --build --preset windows-x86-msvc-release-stage
if errorlevel 1 exit /b %errorlevel%

echo [INFO] Installing runtime binaries to stage/bin ...
call "%CMAKE_WRAPPER%" --build --preset windows-x86-msvc-release-stage-install
if errorlevel 1 exit /b %errorlevel%

echo [OK] Build complete. See stage\bin
