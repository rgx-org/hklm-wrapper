if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
  set(_vcpkg_root "$ENV{VCPKG_ROOT}")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(_vcpkg_root "$ENV{HOME}/vcpkg")
else()
  message(FATAL_ERROR
    "Unable to locate vcpkg: VCPKG_ROOT is not set and HOME is unavailable.\n"
    "Set VCPKG_ROOT to your vcpkg clone path.")
endif()

set(_vcpkg_toolchain "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
if(NOT EXISTS "${_vcpkg_toolchain}")
  message(FATAL_ERROR
    "vcpkg toolchain not found at: ${_vcpkg_toolchain}\n"
    "Set VCPKG_ROOT to your vcpkg clone path or install vcpkg at $HOME/vcpkg.")
endif()

include("${_vcpkg_toolchain}")
