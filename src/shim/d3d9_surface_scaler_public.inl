bool InstallD3D9SurfaceScalerHooks() {
  LogConfigOnceIfNeeded();
  if (!IsScalingEnabled()) {
    g_active.store(false, std::memory_order_release);
    g_hooksInstalled.store(false, std::memory_order_release);
    return true;
  }

  // dgVoodoo (and similar wrappers) can route D3D9 through other backends.
  // The shim's present/backbuffer scaling hooks are fragile there; prefer a
  // dgVoodoo AddOn that can see the real backend resources.
  if (IsDgVoodooPresent()) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
      D3D9Tracef("dgVoodoo detected; shim D3D9 surface scaling disabled (use dgVoodoo AddOn)");
    }
    g_active.store(false, std::memory_order_release);
    g_hooksInstalled.store(false, std::memory_order_release);
    return true;
  }

  bool expected = false;
  if (!g_active.compare_exchange_strong(expected, true)) {
    return true;
  }

  g_stopInitThread.store(false, std::memory_order_release);
  g_initThread = CreateThread(nullptr, 0, &D3D9InitThreadProc, nullptr, 0, nullptr);
  if (!g_initThread) {
    D3D9Tracef("failed to start init thread");
    g_active.store(false, std::memory_order_release);
    return false;
  }
  {
    const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
    D3D9Tracef("install requested (waiting for d3d9.dll; scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));
  }
  return true;
}

bool AreD3D9SurfaceScalerHooksActive() {
  return g_hooksInstalled.load(std::memory_order_acquire);
}

void RemoveD3D9SurfaceScalerHooks() {
  if (!g_active.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  g_hooksInstalled.store(false, std::memory_order_release);

  g_stopInitThread.store(true, std::memory_order_release);
  HANDLE th = g_initThread;
  g_initThread = nullptr;
  if (th) {
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
  }

  // Release swapchains.
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    for (auto& kv : g_deviceStates) {
      SafeRelease(kv.second.swapchain);
    }
    g_deviceStates.clear();
  }

  // Disable hooks we installed.
  void* tgt = g_targetPresent.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetReset.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetCreateDeviceEx.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }
  tgt = g_targetCreateDevice.exchange(nullptr);
  if (tgt) {
    (void)MH_DisableHook(tgt);
    (void)MH_RemoveHook(tgt);
  }

  // Note: export hooks are disabled via MH_ALL_HOOKS during registry hook teardown,
  // but in case registry hooks are disabled we still release MinHook here.
  g_fpDirect3DCreate9 = nullptr;
  g_fpDirect3DCreate9Ex = nullptr;

  g_fpCreateDevice = nullptr;
  g_fpCreateDeviceEx = nullptr;
  g_fpReset = nullptr;
  g_fpPresent = nullptr;

  ReleaseMinHook();
}
