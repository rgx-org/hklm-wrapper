#include "shim/ddraw_surface_scaler.h"

#include "shim/surface_scale_config.h"

#include "shim/window_scale_registry.h"

#include "shim/minhook_runtime.h"

#include <MinHook.h>

#include <windows.h>
#include <unknwn.h>
#include <ddraw.h>

#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace twinshim {
namespace {

#include "shim/ddraw_surface_scaler_utils.inl"

#include "shim/ddraw_surface_scaler_scaler.inl"

#include "shim/ddraw_surface_scaler_hooks.inl"

} // namespace

#include "shim/ddraw_surface_scaler_public.inl"

} // namespace twinshim
