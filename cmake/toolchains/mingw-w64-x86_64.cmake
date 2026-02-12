cmake_minimum_required(VERSION 3.20)

# MinGW-w64 cross toolchain for 64-bit Windows targets.
# Not strictly required for Win32, but useful when you need x64 builds.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_mingw_prefix x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${_mingw_prefix}-gcc)
set(CMAKE_CXX_COMPILER ${_mingw_prefix}-g++)
set(CMAKE_RC_COMPILER  ${_mingw_prefix}-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)
