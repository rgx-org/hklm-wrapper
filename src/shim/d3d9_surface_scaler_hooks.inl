static IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk) {
  IDirect3D9* d3d9 = g_fpDirect3DCreate9 ? g_fpDirect3DCreate9(sdk) : nullptr;
  if (d3d9) {
    (void)EnsureCreateDeviceHookInstalled(d3d9);
  }
  return d3d9;
}

static HRESULT WINAPI Hook_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
  if (!g_fpDirect3DCreate9Ex) {
    return E_FAIL;
  }
  HRESULT hr = g_fpDirect3DCreate9Ex(sdk, out);
  if (SUCCEEDED(hr) && out && *out) {
    (void)EnsureCreateDeviceHookInstalled(*out);
    (void)EnsureCreateDeviceExHookInstalled(*out);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self,
                                                   UINT Adapter,
                                                   D3DDEVTYPE DeviceType,
                                                   HWND hFocusWindow,
                                                   DWORD BehaviorFlags,
                                                   D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                   IDirect3DDevice9** ppReturnedDeviceInterface) {
  LogConfigOnceIfNeeded();
  if (!g_fpCreateDevice) {
    return E_FAIL;
  }

  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled() || !pPresentationParameters || !ppReturnedDeviceInterface) {
    return g_fpCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
  }

  // Only apply in windowed mode.
  if (!pPresentationParameters->Windowed) {
    bool expected = false;
    if (g_loggedFullscreenSkip.compare_exchange_strong(expected, true)) {
      D3D9Tracef("CreateDevice: fullscreen detected -> surface scaling disabled (windowed-only)");
    }
    return g_fpCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
  }

  HWND hwnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : hFocusWindow;

  D3DPRESENT_PARAMETERS ppCopy = *pPresentationParameters;

  UINT srcW = ppCopy.BackBufferWidth;
  UINT srcH = ppCopy.BackBufferHeight;
  if (srcW == 0 || srcH == 0) {
    // In windowed mode, some apps pass 0 and rely on implicit sizing.
    (void)GetClientSize(hwnd, &srcW, &srcH);
  }

  UINT dstW = CalcScaledUInt(srcW, cfg.factor);
  UINT dstH = CalcScaledUInt(srcH, cfg.factor);

  HRESULT hr = g_fpCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, &ppCopy, ppReturnedDeviceInterface);
  if (FAILED(hr) || !*ppReturnedDeviceInterface) {
    return hr;
  }

  IDirect3DDevice9* dev = *ppReturnedDeviceInterface;
  (void)EnsureDeviceHooksInstalledFromVTable(dev);

  const bool resized = SetWindowClientSize(hwnd, dstW, dstH);
  D3D9Tracef("CreateDevice: scale resize window client -> %ux%u (scale=%.3f, %s)", dstW, dstH, cfg.factor, resized ? "ok" : "failed");

  // Register scaling info for mouse coordinate mapping hooks.
  {
    UINT winW = 0, winH = 0;
    if (GetClientSize(hwnd, &winW, &winH)) {
      twinshim::RegisterScaledWindow(hwnd, (int)srcW, (int)srcH, (int)winW, (int)winH, cfg.factor);
    } else {
      twinshim::RegisterScaledWindow(hwnd, (int)srcW, (int)srcH, (int)dstW, (int)dstH, cfg.factor);
    }
  }

  (void)UpdateStateForDevice(dev, true, cfg.factor, cfg.method, hwnd, srcW, srcH, dstW, dstH);

  {
    UINT winW = 0, winH = 0;
    (void)GetClientSize(hwnd, &winW, &winH);
    D3D9Tracef("CreateDevice: scaling=1 window=%p client=%ux%u src=%ux%u dst=%ux%u scale=%.3f method=%ls bb=%ux%u windowed=1",
               (void*)hwnd,
               winW,
               winH,
               srcW,
               srcH,
               dstW,
               dstH,
               cfg.factor,
               SurfaceScaleMethodToString(cfg.method),
               (unsigned)ppCopy.BackBufferWidth,
               (unsigned)ppCopy.BackBufferHeight);
    MarkLoggedCreate(dev);
  }

  // Build swapchain now (best effort).
  DeviceState snapshot;
  if (TryGetState(dev, &snapshot)) {
    DeviceState local = snapshot;
    if (CreateOrResizeSwapChain(dev, local)) {
      IDirect3DSwapChain9* sc = local.swapchain;
      local.swapchain = nullptr;
      UpdateSwapChainPointer(dev, sc);
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDeviceEx(IDirect3D9Ex* self,
                                                     UINT Adapter,
                                                     D3DDEVTYPE DeviceType,
                                                     HWND hFocusWindow,
                                                     DWORD BehaviorFlags,
                                                     D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                     D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                     IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
  LogConfigOnceIfNeeded();
  if (!g_fpCreateDeviceEx) {
    return E_FAIL;
  }
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  if (!IsScalingEnabled() || !pPresentationParameters || !ppReturnedDeviceInterface) {
    return g_fpCreateDeviceEx(self,
                              Adapter,
                              DeviceType,
                              hFocusWindow,
                              BehaviorFlags,
                              pPresentationParameters,
                              pFullscreenDisplayMode,
                              ppReturnedDeviceInterface);
  }
  if (!pPresentationParameters->Windowed) {
    bool expected = false;
    if (g_loggedFullscreenSkip.compare_exchange_strong(expected, true)) {
      D3D9Tracef("CreateDeviceEx: fullscreen detected -> surface scaling disabled (windowed-only)");
    }
    return g_fpCreateDeviceEx(self,
                              Adapter,
                              DeviceType,
                              hFocusWindow,
                              BehaviorFlags,
                              pPresentationParameters,
                              pFullscreenDisplayMode,
                              ppReturnedDeviceInterface);
  }

  HWND hwnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : hFocusWindow;

  D3DPRESENT_PARAMETERS ppCopy = *pPresentationParameters;

  UINT srcW = ppCopy.BackBufferWidth;
  UINT srcH = ppCopy.BackBufferHeight;
  if (srcW == 0 || srcH == 0) {
    (void)GetClientSize(hwnd, &srcW, &srcH);
  }

  UINT dstW = CalcScaledUInt(srcW, cfg.factor);
  UINT dstH = CalcScaledUInt(srcH, cfg.factor);

  HRESULT hr = g_fpCreateDeviceEx(self,
                                  Adapter,
                                  DeviceType,
                                  hFocusWindow,
                                  BehaviorFlags,
                                  &ppCopy,
                                  pFullscreenDisplayMode,
                                  ppReturnedDeviceInterface);
  if (FAILED(hr) || !*ppReturnedDeviceInterface) {
    return hr;
  }

  IDirect3DDevice9* dev = *ppReturnedDeviceInterface;
  (void)EnsureDeviceHooksInstalledFromVTable(dev);

  const bool resized = SetWindowClientSize(hwnd, dstW, dstH);
  D3D9Tracef("CreateDeviceEx: scale resize window client -> %ux%u (scale=%.3f, %s)", dstW, dstH, cfg.factor, resized ? "ok" : "failed");

  // Register scaling info for mouse coordinate mapping hooks.
  {
    UINT winW = 0, winH = 0;
    if (GetClientSize(hwnd, &winW, &winH)) {
      twinshim::RegisterScaledWindow(hwnd, (int)srcW, (int)srcH, (int)winW, (int)winH, cfg.factor);
    } else {
      twinshim::RegisterScaledWindow(hwnd, (int)srcW, (int)srcH, (int)dstW, (int)dstH, cfg.factor);
    }
  }

  (void)UpdateStateForDevice(dev, true, cfg.factor, cfg.method, hwnd, srcW, srcH, dstW, dstH);
  {
    UINT winW = 0, winH = 0;
    (void)GetClientSize(hwnd, &winW, &winH);
    D3D9Tracef("CreateDeviceEx: scaling=1 window=%p client=%ux%u src=%ux%u dst=%ux%u scale=%.3f method=%ls bb=%ux%u windowed=1",
               (void*)hwnd,
               winW,
               winH,
               srcW,
               srcH,
               dstW,
               dstH,
               cfg.factor,
               SurfaceScaleMethodToString(cfg.method),
               (unsigned)ppCopy.BackBufferWidth,
               (unsigned)ppCopy.BackBufferHeight);
    MarkLoggedCreate(dev);
  }
  DeviceState snapshot;
  if (TryGetState(dev, &snapshot)) {
    DeviceState local = snapshot;
    if (CreateOrResizeSwapChain(dev, local)) {
      IDirect3DSwapChain9* sc = local.swapchain;
      local.swapchain = nullptr;
      UpdateSwapChainPointer(dev, sc);
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters) {
  if (!g_fpReset) {
    return D3DERR_INVALIDCALL;
  }
  LogConfigOnceIfNeeded();

  DeviceState st;
  const bool tracked = TryGetState(device, &st);
  if (!tracked || !st.scalingEnabled || !IsScalingEnabled()) {
    return g_fpReset(device, pPresentationParameters);
  }

  // Drop swapchain before reset.
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto it = g_deviceStates.find(device);
    if (it != g_deviceStates.end()) {
      SafeRelease(it->second.swapchain);
    }
  }

  HRESULT hr = g_fpReset(device, pPresentationParameters);
  if (FAILED(hr)) {
    return hr;
  }

  // Rebuild swapchain sizes.
  UINT srcW = 0, srcH = 0;
  if (pPresentationParameters) {
    srcW = pPresentationParameters->BackBufferWidth;
    srcH = pPresentationParameters->BackBufferHeight;
  }
  if (srcW == 0 || srcH == 0) {
    (void)GetClientSize(st.hwnd, &srcW, &srcH);
  }
  if (srcW == 0 || srcH == 0) {
    return hr;
  }

  UINT dstW = CalcScaledUInt(srcW, st.scaleFactor);
  UINT dstH = CalcScaledUInt(srcH, st.scaleFactor);

  (void)SetWindowClientSize(st.hwnd, dstW, dstH);

  (void)UpdateStateForDevice(device, true, st.scaleFactor, st.scaleMethod, st.hwnd, srcW, srcH, dstW, dstH);
  DeviceState snapshot;
  if (TryGetState(device, &snapshot)) {
    DeviceState local = snapshot;
    if (CreateOrResizeSwapChain(device, local)) {
      IDirect3DSwapChain9* sc = local.swapchain;
      local.swapchain = nullptr;
      UpdateSwapChainPointer(device, sc);
    }
  }

  return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* device,
                                              CONST RECT* pSourceRect,
                                              CONST RECT* pDestRect,
                                              HWND hDestWindowOverride,
                                              CONST RGNDATA* pDirtyRegion) {
  if (!g_fpPresent) {
    return D3DERR_INVALIDCALL;
  }
  LogConfigOnceIfNeeded();

  if (!IsScalingEnabled()) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  DeviceState st;
  if (!TryGetState(device, &st) || !st.scalingEnabled) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  // If the app is presenting to a different window, don't interfere.
  if (hDestWindowOverride && st.hwnd && hDestWindowOverride != st.hwnd) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  // Grab current swapchain pointer.
  IDirect3DSwapChain9* sc = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto it = g_deviceStates.find(device);
    if (it != g_deviceStates.end() && it->second.swapchain) {
      sc = it->second.swapchain;
      sc->AddRef();
    }
  }

  if (!sc) {
    // Best-effort create (device might have just been reset).
    DeviceState snapshot;
    if (TryGetState(device, &snapshot)) {
      DeviceState local = snapshot;
      if (CreateOrResizeSwapChain(device, local)) {
        sc = local.swapchain;
        local.swapchain = nullptr;
        if (sc) {
          sc->AddRef();
          UpdateSwapChainPointer(device, sc);
        }
      }
    }
  }

  if (!sc) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  if (TryMarkLoggedFirstPresent(device)) {
    D3D9Tracef("Present: scaling active (%ls) src=%ux%u -> dst=%ux%u", SurfaceScaleMethodToString(st.scaleMethod), st.srcW, st.srcH, st.dstW, st.dstH);
  }

  IDirect3DSurface9* src = nullptr;
  HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src);
  if (FAILED(hr) || !src) {
    SafeRelease(src);
    SafeRelease(sc);
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  IDirect3DSurface9* dst = nullptr;
  hr = sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &dst);
  if (FAILED(hr) || !dst) {
    SafeRelease(dst);
    SafeRelease(src);
    SafeRelease(sc);
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  // Filtered upscale.
  D3DTEXTUREFILTERTYPE filter = FilterForMethod(st.scaleMethod);
  hr = device->StretchRect(src, pSourceRect, dst, nullptr, filter);
  if (FAILED(hr) && filter == D3DTEXF_GAUSSIANQUAD) {
    // Fallback: many drivers reject GAUSSIANQUAD for StretchRect.
    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        D3D9Tracef("Present: high-quality filter requested but GAUSSIANQUAD rejected; falling back to linear");
      }
    }
    hr = device->StretchRect(src, pSourceRect, dst, nullptr, D3DTEXF_LINEAR);
  }
  if (FAILED(hr) && st.scaleMethod != SurfaceScaleMethod::kPoint) {
    // Last-chance fallback.
    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        D3D9Tracef("Present: filtered scaling rejected; falling back to point");
      }
    }
    hr = device->StretchRect(src, pSourceRect, dst, nullptr, D3DTEXF_POINT);
  }
  if (FAILED(hr)) {
    SafeRelease(dst);
    SafeRelease(src);
    SafeRelease(sc);
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  HRESULT hrPresent = sc->Present(nullptr, nullptr, nullptr, nullptr, 0);
  SafeRelease(dst);
  SafeRelease(src);
  SafeRelease(sc);

  if (FAILED(hrPresent)) {
    return g_fpPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }
  return D3D_OK;
}

static bool CreateHookApiTyped(LPCWSTR moduleName, LPCSTR procName, LPVOID detour, LPVOID* original) {
  return MH_CreateHookApi(moduleName, procName, detour, original) == MH_OK;
}

static bool InstallD3D9ExportsHooksOnce() {
  LogConfigOnceIfNeeded();
  if (!IsScalingEnabled()) {
    return true;
  }

  if (IsDgVoodooPresent()) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
      D3D9Tracef("dgVoodoo detected; shim D3D9 surface scaling hooks disabled (use dgVoodoo AddOn)");
    }
    return true;
  }

  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  D3D9Tracef("surface scaling hooks enabled (scale=%.3f method=%ls)", cfg.factor, SurfaceScaleMethodToString(cfg.method));

  if (!AcquireMinHook()) {
    D3D9Tracef("AcquireMinHook failed");
    return false;
  }

  bool ok = false;
  // Try both module spellings.
  ok |= CreateHookApiTyped(L"d3d9", "Direct3DCreate9", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9));
  ok |= CreateHookApiTyped(L"d3d9.dll", "Direct3DCreate9", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9));

  // Optional (Vista+).
  (void)CreateHookApiTyped(L"d3d9", "Direct3DCreate9Ex", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9Ex), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9Ex));
  (void)CreateHookApiTyped(L"d3d9.dll", "Direct3DCreate9Ex", reinterpret_cast<LPVOID>(&Hook_Direct3DCreate9Ex), reinterpret_cast<LPVOID*>(&g_fpDirect3DCreate9Ex));

  if (!ok) {
    D3D9Tracef("failed to hook Direct3DCreate9 exports (d3d9.dll not hookable yet?)");
    ReleaseMinHook();
    return false;
  }

  if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
    D3D9Tracef("MH_EnableHook(MH_ALL_HOOKS) failed");
    ReleaseMinHook();
    return false;
  }
  g_hooksInstalled.store(true, std::memory_order_release);
  D3D9Tracef("Direct3DCreate9 export hooks installed");
  return true;
}

static DWORD WINAPI D3D9InitThreadProc(LPVOID) {
  // Wait until d3d9 is present in the process; then install export hooks.
  // 50ms * 12000 ~= 10 minutes.
  for (int i = 0; i < 12000 && !g_stopInitThread.load(std::memory_order_acquire); i++) {
    if ((i % 20) == 0) {
      ProbeLogCommonGraphicsModules();
      // After ~5 seconds, if we still haven't observed d3d9.dll, dump a filtered module snapshot.
      if (i == 100 && !g_seenD3D9.load(std::memory_order_acquire)) {
        ProbeDumpInterestingModulesOnce();
      }
    }
    if (GetModuleHandleW(L"d3d9.dll") != nullptr || GetModuleHandleW(L"d3d9") != nullptr) {
      break;
    }
    Sleep(50);
  }

  if (!g_stopInitThread.load(std::memory_order_acquire)) {
    const bool ok = InstallD3D9ExportsHooksOnce();
    D3D9Tracef("init thread finished (ok=%d)", ok ? 1 : 0);
    if (!ok && !g_seenD3D9.load(std::memory_order_acquire)) {
      D3D9Tracef("d3d9.dll not detected; likely not a D3D9 path (check snapshot above)");
    }
  }

  return 0;
}
