#pragma once

namespace twinshim {

// Installs best-effort mouse coordinate scaling hooks when --scale is enabled.
// The hooks translate between the physical (scaled) window client pixels and the
// app's logical (pre-scale) coordinate space.
bool InstallMouseScaleHooks();
void RemoveMouseScaleHooks();

} // namespace twinshim
