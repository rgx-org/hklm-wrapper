#pragma once

#include <windows.h>

namespace twinshim {

bool InstallRegistryHooks();
bool AreRegistryHooksActive();
void RemoveRegistryHooks();

}
