@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~1"=="" (
  echo [ERROR] Missing CMake arguments.
  echo Usage: %~nx0 ^<cmake args...^>
  echo Example: %~nx0 --preset windows-x86-msvc-release
  exit /b 2
)

set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)

if "%VSINSTALL%"=="" (
  for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$root='C:\ProgramData\Microsoft\VisualStudio'; if (Test-Path $root) { Get-ChildItem -Path $root -Recurse -Filter state.json -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | ForEach-Object { try { $json = Get-Content $_.FullName -Raw | ConvertFrom-Json; $path = $json.installationPath; if ($path -and (Test-Path (Join-Path $path 'Common7\Tools\VsDevCmd.bat'))) { $path; break } } catch {} } }"`) do (
    if "%%I" NEQ "" set "VSINSTALL=%%I"
  )
)

if "%VSINSTALL%"=="" (
  echo [ERROR] Could not find a Visual Studio installation with C++ tools.
  echo Tried: vswhere and C:\ProgramData\Microsoft\VisualStudio metadata fallback.
  echo Ensure "Desktop development with C++" is installed.
  exit /b 1
)

set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo [ERROR] VsDevCmd.bat not found at "%VSDEVCMD%".
  exit /b 1
)

call "%VSDEVCMD%" -arch=x86 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=%USERPROFILE%\vcpkg"

cmake %*
exit /b %errorlevel%
