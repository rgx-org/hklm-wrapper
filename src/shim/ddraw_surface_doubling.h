#pragma once

namespace hklmwrap {

// DirectDraw-based implementation of surface doubling (system ddraw.dll paths).
//
// Note: if the process is using an app-local/wrapper ddraw.dll (dgVoodoo/etc),
// the shim intentionally disables this hook. Use a dgVoodoo AddOn for scaling
// in wrapper-backed paths.
//
// Controlled by target process command-line options:
//   --scale <1.1-100>
//   --scale-method <point|bilinear|bicubic>
bool InstallDDrawSurfaceDoublingHooks();
bool AreDDrawSurfaceDoublingHooksActive();
void RemoveDDrawSurfaceDoublingHooks();

}
