@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%.."
set "CMAKE_WRAPPER=%SCRIPT_DIR%cmake-msvc-x86.cmd"
set "PRESET=native-tests-windows"

if not exist "%CMAKE_WRAPPER%" (
  echo [ERROR] Missing helper script: "%CMAKE_WRAPPER%".
  exit /b 1
)

pushd "%REPO_ROOT%" >nul
if errorlevel 1 (
  echo [ERROR] Failed to cd to repo root: "%REPO_ROOT%".
  exit /b 1
)

echo [INFO] Configuring preset %PRESET% ...
call "%CMAKE_WRAPPER%" --preset %PRESET%
if errorlevel 1 (
  set "ERR=!errorlevel!"
  popd >nul
  exit /b !ERR!
)

echo [INFO] Building preset %PRESET% ...
call "%CMAKE_WRAPPER%" --build --preset %PRESET%
if errorlevel 1 (
  set "ERR=!errorlevel!"
  popd >nul
  exit /b !ERR!
)

echo [INFO] Running CTest preset %PRESET% ...
ctest --preset %PRESET%
if errorlevel 1 (
  set "ERR=!errorlevel!"
  popd >nul
  exit /b !ERR!
)

popd >nul
echo [OK] Tests complete.
exit /b 0
