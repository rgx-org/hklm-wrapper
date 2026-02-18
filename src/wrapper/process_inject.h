#pragma once

#include <string>

#include <windows.h>

namespace twinshim {

bool InjectDllIntoProcess(HANDLE processHandle, const std::wstring& dllPath);

}
