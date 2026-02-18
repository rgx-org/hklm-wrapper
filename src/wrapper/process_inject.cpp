#include "wrapper/process_inject.h"

#include <windows.h>

namespace twinshim {

bool InjectDllIntoProcess(HANDLE processHandle, const std::wstring& dllPath) {
  if (!processHandle || dllPath.empty()) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }

  const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(processHandle, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) {
    return false;
  }

  SIZE_T written = 0;
  if (!WriteProcessMemory(processHandle, remote, dllPath.c_str(), bytes, &written) || written != bytes) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (!kernel32) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }
  auto loadLibraryW = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");
  if (!loadLibraryW) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }

  HANDLE thread = CreateRemoteThread(processHandle, nullptr, 0, loadLibraryW, remote, 0, nullptr);
  if (!thread) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }

  const DWORD waitResult = WaitForSingleObject(thread, 15000);
  if (waitResult != WAIT_OBJECT_0) {
    CloseHandle(thread);
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    if (waitResult == WAIT_TIMEOUT) {
      SetLastError(ERROR_TIMEOUT);
    }
    return false;
  }

  DWORD exitCode = 0;
  if (!GetExitCodeThread(thread, &exitCode)) {
    CloseHandle(thread);
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }

  CloseHandle(thread);
  VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
  if (exitCode == 0) {
    SetLastError(ERROR_DLL_INIT_FAILED);
    return false;
  }
  return true;
}

}
