
// dgVoodoo's documentation and samples historically used different spellings.
// Export both to maximize compatibility.
extern "C" {

__declspec(dllexport) bool API_EXPORT AddOnInit(dgVoodoo::IAddonMainCallback* pAddonMain) {
  return AddonInitCommon(pAddonMain);
}

__declspec(dllexport) void API_EXPORT AddOnExit() {
  AddonExitCommon();
}

__declspec(dllexport) bool API_EXPORT AddonInit(dgVoodoo::IAddonMainCallback* pAddonMain) {
  return AddonInitCommon(pAddonMain);
}

__declspec(dllexport) void API_EXPORT AddonExit() {
  AddonExitCommon();
}

}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)lpvReserved;
  if (fdwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinstDLL);
  }
  return TRUE;
}
