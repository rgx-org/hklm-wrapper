#pragma once

#include <windows.h>

#include <string>

namespace twinshim {

class InternalDispatchGuard {
 public:
  InternalDispatchGuard();
  ~InternalDispatchGuard();
};

std::wstring FormatRegType(DWORD type);
std::wstring FormatValuePreview(DWORD type, const BYTE* data, DWORD cbData);

// Returns true when registry API tracing is enabled for the given API name.
// This is intended for guarding expensive debug string construction at call sites.
bool IsRegistryTraceEnabledForApi(const wchar_t* apiName);

void TraceApiEvent(const wchar_t* apiName,
                   const wchar_t* opType,
                   const std::wstring& keyPath,
                   const std::wstring& valueName,
                   const std::wstring& valueData);

LONG TraceReadResultAndReturn(const wchar_t* apiName,
                              const std::wstring& keyPath,
                              const std::wstring& valueName,
                              LONG status,
                              bool typeKnown,
                              DWORD type,
                              const BYTE* data,
                              DWORD cbData,
                              bool sizeOnly);

LONG TraceEnumReadResultAndReturn(const wchar_t* apiName,
                                  const std::wstring& keyPath,
                                  DWORD index,
                                  const std::wstring& valueName,
                                  LONG status,
                                  bool typeKnown,
                                  DWORD type,
                                  const BYTE* data,
                                  DWORD cbData,
                                  bool sizeOnly);

}
