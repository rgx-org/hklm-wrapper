cmake_minimum_required(VERSION 3.20)

# MinGW-w64 cross toolchain for 32-bit Windows (Win32) targets.
# Intended to be used via vcpkg chainloading:
#   -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
#   -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=<this file>

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

# Prefer explicit target triplet prefix.
set(_mingw_prefix i686-w64-mingw32)

set(CMAKE_C_COMPILER   ${_mingw_prefix}-gcc)
set(CMAKE_CXX_COMPILER ${_mingw_prefix}-g++)
set(CMAKE_RC_COMPILER  ${_mingw_prefix}-windres)

# Ensure CMake searches headers/libs in the target root and not the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Avoid accidentally producing import libs without a DLL when building shared libs.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)
