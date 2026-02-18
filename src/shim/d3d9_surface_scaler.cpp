#include "shim/d3d9_surface_scaler.h"

#include "shim/minhook_runtime.h"
#include "shim/surface_scale_config.h"
#include "shim/window_scale_registry.h"

#include <MinHook.h>

#include <windows.h>

#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <cstdio>

#include <tlhelp32.h>

namespace twinshim {
namespace {

#include "shim/d3d9_surface_scaler_state.inl"

#include "shim/d3d9_surface_scaler_hooks.inl"

}

#include "shim/d3d9_surface_scaler_public.inl"

}
