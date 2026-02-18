bool InstallDDrawSurfaceScalerHooks() {
  if (!IsScalingEnabled()) {
    g_active.store(false, std::memory_order_release);
    return true;
  }

  // If ddraw.dll is already loaded and it's not the system DLL, assume a wrapper
  // (dgVoodoo/etc) and do not install DirectDraw scaling hooks.
  if (GetModuleHandleW(L"ddraw.dll") != nullptr && IsLikelyWrapperDDrawDll()) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
      Tracef("ddraw.dll wrapper detected at install time; shim DirectDraw scaling hooks disabled (use dgVoodoo AddOn)");
    }
    g_active.store(false, std::memory_order_release);
    return true;
  }

  bool expected = false;
  if (!g_active.compare_exchange_strong(expected, true)) {
    return true;
  }

  g_stopInitThread.store(false, std::memory_order_release);
  g_initThread = CreateThread(nullptr, 0, &DDrawInitThreadProc, nullptr, 0, nullptr);
  if (!g_initThread) {
    Tracef("failed to start init thread");
    g_active.store(false, std::memory_order_release);
    return false;
  }
  {
    const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
    Tracef("install requested (waiting for ddraw.dll; scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));
  }
  return true;
}

bool AreDDrawSurfaceScalerHooksActive() {
  return g_active.load(std::memory_order_acquire);
}

void RemoveDDrawSurfaceScalerHooks() {
  if (!g_active.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  g_stopInitThread.store(true, std::memory_order_release);
  HANDLE th = g_initThread;
  g_initThread = nullptr;
  if (th) {
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
  }

  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    SafeRelease(g_primary);
    SafeRelease(g_cachedBackbuffer);
    g_cachedBackW = 0;
    g_cachedBackH = 0;
    g_hwnd = nullptr;
    g_coopFlags = 0;
    g_resizedOnce = false;
  }

  // Release any D3D9 resources used for filtered scaling.
  g_d3d9Scaler.Shutdown();

  void* tgtFlip = g_targetDDS7_Flip.exchange(nullptr);
  if (tgtFlip) {
    (void)MH_DisableHook(tgtFlip);
    (void)MH_RemoveHook(tgtFlip);
  }
  void* tgtBlt = g_targetDDS7_Blt.exchange(nullptr);
  if (tgtBlt) {
    (void)MH_DisableHook(tgtBlt);
    (void)MH_RemoveHook(tgtBlt);
  }
  void* tgtBltFast = g_targetDDS7_BltFast.exchange(nullptr);
  if (tgtBltFast) {
    (void)MH_DisableHook(tgtBltFast);
    (void)MH_RemoveHook(tgtBltFast);
  }
  void* tgt = g_targetDD7_CreateSurface.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetDD7_SetCooperativeLevel.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }

  g_fpDD7_SetCooperativeLevel = nullptr;
  g_fpDD7_CreateSurface = nullptr;
  g_fpDDS7_Flip = nullptr;
    g_fpDDS7_Blt = nullptr;
    g_fpDDS7_BltFast = nullptr;
  g_fpDirectDrawCreate = nullptr;
  g_fpDirectDrawCreateEx = nullptr;

  ReleaseMinHook();
}
