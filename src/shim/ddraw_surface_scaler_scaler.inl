struct PixelFormatInfo {
  uint32_t rMask = 0;
  uint32_t gMask = 0;
  uint32_t bMask = 0;
  uint32_t aMask = 0;
  int rShift = 0;
  int gShift = 0;
  int bShift = 0;
  int aShift = 0;
  int rBits = 0;
  int gBits = 0;
  int bBits = 0;
  int aBits = 0;
  int bytesPerPixel = 0;
};

static int CountBits(uint32_t v) {
  int c = 0;
  while (v) {
    c += (int)(v & 1u);
    v >>= 1;
  }
  return c;
}

static int CountTrailingZeros(uint32_t v) {
  if (v == 0) {
    return 0;
  }
  int c = 0;
  while ((v & 1u) == 0) {
    c++;
    v >>= 1;
  }
  return c;
}

static bool GetPixelFormatInfoFromSurface(LPDIRECTDRAWSURFACE7 surf, PixelFormatInfo* out) {
  if (!surf || !out) {
    return false;
  }
  DDSURFACEDESC2 sd{};
  sd.dwSize = sizeof(sd);
  if (FAILED(surf->GetSurfaceDesc(&sd))) {
    return false;
  }
  if ((sd.ddpfPixelFormat.dwFlags & DDPF_RGB) == 0) {
    return false;
  }

  PixelFormatInfo info;
  info.rMask = sd.ddpfPixelFormat.dwRBitMask;
  info.gMask = sd.ddpfPixelFormat.dwGBitMask;
  info.bMask = sd.ddpfPixelFormat.dwBBitMask;
  info.aMask = (sd.ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS) ? sd.ddpfPixelFormat.dwRGBAlphaBitMask : 0;
  info.rShift = CountTrailingZeros(info.rMask);
  info.gShift = CountTrailingZeros(info.gMask);
  info.bShift = CountTrailingZeros(info.bMask);
  info.aShift = CountTrailingZeros(info.aMask);
  info.rBits = CountBits(info.rMask);
  info.gBits = CountBits(info.gMask);
  info.bBits = CountBits(info.bMask);
  info.aBits = CountBits(info.aMask);

  const uint32_t bpp = sd.ddpfPixelFormat.dwRGBBitCount;
  if (bpp == 16) {
    info.bytesPerPixel = 2;
    // Some wrappers report 16bpp RGB but leave masks zero. Assume 565.
    if (info.rMask == 0 && info.gMask == 0 && info.bMask == 0) {
      info.rMask = 0xF800;
      info.gMask = 0x07E0;
      info.bMask = 0x001F;
      info.rShift = 11;
      info.gShift = 5;
      info.bShift = 0;
      info.rBits = 5;
      info.gBits = 6;
      info.bBits = 5;
    }
  } else if (bpp == 32) {
    info.bytesPerPixel = 4;
    // Some wrappers report 32bpp RGB but leave masks zero. Assume XRGB8888.
    if (info.rMask == 0 && info.gMask == 0 && info.bMask == 0) {
      info.rMask = 0x00FF0000;
      info.gMask = 0x0000FF00;
      info.bMask = 0x000000FF;
      info.aMask = 0;
      info.rShift = 16;
      info.gShift = 8;
      info.bShift = 0;
      info.rBits = 8;
      info.gBits = 8;
      info.bBits = 8;
      info.aBits = 0;
    }
  } else {
    return false;
  }

  // If we still don't have masks/bits, bail out.
  if (info.rMask == 0 || info.gMask == 0 || info.bMask == 0 || info.rBits == 0 || info.gBits == 0 || info.bBits == 0) {
    return false;
  }

  *out = info;
  return true;
}

static uint8_t ExpandTo8(uint32_t v, int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 8) {
    return (uint8_t)std::min<uint32_t>(255u, v);
  }
  const uint32_t maxv = (1u << (uint32_t)bits) - 1u;
  return (uint8_t)((v * 255u + (maxv / 2u)) / maxv);
}

static void UnpackRGBA(const PixelFormatInfo& fmt, uint32_t px, uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a) {
  const uint32_t rv = (fmt.rMask ? ((px & fmt.rMask) >> (uint32_t)fmt.rShift) : 0);
  const uint32_t gv = (fmt.gMask ? ((px & fmt.gMask) >> (uint32_t)fmt.gShift) : 0);
  const uint32_t bv = (fmt.bMask ? ((px & fmt.bMask) >> (uint32_t)fmt.bShift) : 0);
  const uint32_t av = (fmt.aMask ? ((px & fmt.aMask) >> (uint32_t)fmt.aShift) : 255u);
  if (r) {
    *r = ExpandTo8(rv, fmt.rBits);
  }
  if (g) {
    *g = ExpandTo8(gv, fmt.gBits);
  }
  if (b) {
    *b = ExpandTo8(bv, fmt.bBits);
  }
  if (a) {
    *a = fmt.aMask ? ExpandTo8(av, fmt.aBits) : 255;
  }
}

static uint32_t ReadPixel(const uint8_t* base, int pitch, int x, int y, int bpp) {
  const uint8_t* p = base + (ptrdiff_t)y * pitch + (ptrdiff_t)x * bpp;
  if (bpp == 4) {
    return *(const uint32_t*)p;
  }
  return (uint32_t)(*(const uint16_t*)p);
}

// --- D3D9-based filtered scaling (hardware accelerated) ---
//
// DirectDraw surfaces from wrappers (e.g. dgVoodoo) can expose GetDC, but using GDI
// StretchBlt every frame is often very slow. For bilinear/bicubic, we instead:
//   1) Lock() the source surface (read-only)
//   2) Convert to A8R8G8B8 in a CPU buffer
//   3) Upload to a dynamic D3D9 texture
//   4) Render to the game window with:
//        - bilinear: fixed-function sampling with linear filtering
//        - bicubic: two-pass 1D cubic filter via pixel shaders
// If anything fails, callers fall back to point stretching.

struct ID3DBlob : public IUnknown {
  virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
  virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using D3DCompile_t = HRESULT(WINAPI*)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const void* pDefines,
    void* pInclude,
    LPCSTR pEntryPoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs);

class DDrawD3D9Scaler {
 public:
  bool PresentScaled(LPDIRECTDRAWSURFACE7 srcSurf,
                     const RECT& srcRect,
                     HWND hwnd,
                     UINT dstW,
                     UINT dstH,
                     SurfaceScaleMethod method) {
    if (!srcSurf || !hwnd || dstW == 0 || dstH == 0) {
      return false;
    }
    const bool wantLinear = (method == SurfaceScaleMethod::kBilinear || method == SurfaceScaleMethod::kPixelFast);
    const bool wantCubic = (method == SurfaceScaleMethod::kBicubic || method == SurfaceScaleMethod::kCatmullRom || method == SurfaceScaleMethod::kLanczos || method == SurfaceScaleMethod::kLanczos3);
    if (!wantLinear && !wantCubic) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (!EnsureDeviceUnlocked(hwnd, dstW, dstH)) {
      return false;
    }

    // Clamp srcRect to the source surface bounds.
    DDSURFACEDESC2 sd{};
    sd.dwSize = sizeof(sd);
    if (FAILED(srcSurf->GetSurfaceDesc(&sd)) || sd.dwWidth == 0 || sd.dwHeight == 0) {
      return false;
    }

    RECT rc = srcRect;
    rc.left = std::max<LONG>(0, rc.left);
    rc.top = std::max<LONG>(0, rc.top);
    rc.right = std::min<LONG>((LONG)sd.dwWidth, rc.right);
    rc.bottom = std::min<LONG>((LONG)sd.dwHeight, rc.bottom);

    const int srcW = (int)(rc.right - rc.left);
    const int srcH = (int)(rc.bottom - rc.top);
    if (srcW <= 0 || srcH <= 0) {
      return false;
    }

    if (!EnsureSrcTextureUnlocked((UINT)srcW, (UINT)srcH)) {
      return false;
    }
    if (!UploadSurfaceRectToSrcTextureUnlocked(srcSurf, rc, (UINT)srcW, (UINT)srcH)) {
      return false;
    }

    HRESULT hr = D3D_OK;
    if (wantLinear) {
      hr = RenderSinglePassUnlocked(srcTex_, dstW, dstH, /*linear=*/true);
      if (SUCCEEDED(hr)) {
        hr = dev_->Present(nullptr, nullptr, nullptr, nullptr);
      }
      return SUCCEEDED(hr);
    }

    // Bicubic: two-pass separable cubic filter (4 taps per pass).
    if (!EnsureBicubicShadersUnlocked()) {
      return false;
    }
    if (!EnsureIntermediateUnlocked(dstW, (UINT)srcH)) {
      return false;
    }

    // Pass 1: horizontal cubic scaling to (dstW x srcH) render target.
    IDirect3DSurface9* interRt = nullptr;
    hr = interTex_->GetSurfaceLevel(0, &interRt);
    if (FAILED(hr) || !interRt) {
      SafeRelease(interRt);
      return false;
    }

    IDirect3DSurface9* prevRt = nullptr;
    hr = dev_->GetRenderTarget(0, &prevRt);
    if (FAILED(hr) || !prevRt) {
      SafeRelease(interRt);
      SafeRelease(prevRt);
      return false;
    }

    hr = dev_->SetRenderTarget(0, interRt);
    SafeRelease(interRt);
    if (FAILED(hr)) {
      SafeRelease(prevRt);
      return false;
    }

    if (FAILED(RenderCubicPassUnlocked(srcTex_, psCubicH_, dstW, (UINT)srcH, (UINT)srcW, (UINT)srcH))) {
      (void)dev_->SetRenderTarget(0, prevRt);
      SafeRelease(prevRt);
      return false;
    }

    // Pass 2: vertical cubic scaling to backbuffer (dstW x dstH).
    hr = dev_->SetRenderTarget(0, prevRt);
    SafeRelease(prevRt);
    if (FAILED(hr)) {
      return false;
    }
    if (FAILED(RenderCubicPassUnlocked(interTex_, psCubicV_, dstW, dstH, dstW, (UINT)srcH))) {
      return false;
    }

    hr = dev_->Present(nullptr, nullptr, nullptr, nullptr);
    return SUCCEEDED(hr);
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    ShutdownUnlocked();
  }

 private:
  static constexpr DWORD kQuadFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

  struct QuadVtx {
    float x, y, z, rhw;
    float u, v;
  };

  void ShutdownUnlocked() {
    SafeRelease(psCubicH_);
    SafeRelease(psCubicV_);
    SafeRelease(interTex_);
    interW_ = 0;
    interH_ = 0;
    SafeRelease(srcTex_);
    srcW_ = 0;
    srcH_ = 0;
    SafeRelease(dev_);
    SafeRelease(d3d_);
    hwnd_ = nullptr;
    bbW_ = 0;
    bbH_ = 0;
    staging_.clear();

    fpCreate9_ = nullptr;
    if (d3d9Mod_) {
      FreeLibrary(d3d9Mod_);
      d3d9Mod_ = nullptr;
    }

    fpCompile_ = nullptr;
    if (compilerMod_) {
      FreeLibrary(compilerMod_);
      compilerMod_ = nullptr;
    }
    shadersTried_ = false;
  }

  bool EnsureD3D9LoadedUnlocked() {
    if (fpCreate9_) {
      return true;
    }
    if (!d3d9Mod_) {
      d3d9Mod_ = LoadLibraryW(L"d3d9.dll");
      if (!d3d9Mod_) {
        return false;
      }
    }
    fpCreate9_ = (Direct3DCreate9_t)GetProcAddress(d3d9Mod_, "Direct3DCreate9");
    return fpCreate9_ != nullptr;
  }

  bool EnsureDeviceUnlocked(HWND hwnd, UINT bbW, UINT bbH) {
    if (!EnsureD3D9LoadedUnlocked()) {
      return false;
    }

    const bool needNew = (!dev_ || !d3d_ || hwnd_ != hwnd || bbW_ != bbW || bbH_ != bbH);
    if (!needNew) {
      return true;
    }

    // Recreate everything (simple and robust; resize is infrequent).
    SafeRelease(psCubicH_);
    SafeRelease(psCubicV_);
    SafeRelease(interTex_);
    interW_ = 0;
    interH_ = 0;
    SafeRelease(srcTex_);
    srcW_ = 0;
    srcH_ = 0;
    SafeRelease(dev_);
    SafeRelease(d3d_);

    d3d_ = fpCreate9_(D3D_SDK_VERSION);
    if (!d3d_) {
      return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferWidth = bbW;
    pp.BackBufferHeight = bbH;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    DWORD createFlags = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED | D3DCREATE_HARDWARE_VERTEXPROCESSING;
    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT,
                                    D3DDEVTYPE_HAL,
                                    hwnd,
                                    createFlags,
                                    &pp,
                                    &dev_);
    if (FAILED(hr)) {
      createFlags = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED | D3DCREATE_SOFTWARE_VERTEXPROCESSING;
      hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT,
                              D3DDEVTYPE_HAL,
                              hwnd,
                              createFlags,
                              &pp,
                              &dev_);
    }
    if (FAILED(hr) || !dev_) {
      SafeRelease(dev_);
      SafeRelease(d3d_);
      return false;
    }

    hwnd_ = hwnd;
    bbW_ = bbW;
    bbH_ = bbH;

    // Fixed pipeline state (we re-assert key bits per draw).
    return true;
  }

  bool EnsureSrcTextureUnlocked(UINT w, UINT h) {
    if (srcTex_ && srcW_ == w && srcH_ == h) {
      return true;
    }
    SafeRelease(srcTex_);
    srcW_ = 0;
    srcH_ = 0;
    HRESULT hr = dev_->CreateTexture(w,
                                    h,
                                    1,
                                    D3DUSAGE_DYNAMIC,
                                    D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT,
                                    &srcTex_,
                                    nullptr);
    if (FAILED(hr) || !srcTex_) {
      SafeRelease(srcTex_);
      return false;
    }
    srcW_ = w;
    srcH_ = h;
    return true;
  }

  bool EnsureIntermediateUnlocked(UINT w, UINT h) {
    if (interTex_ && interW_ == w && interH_ == h) {
      return true;
    }
    SafeRelease(interTex_);
    interW_ = 0;
    interH_ = 0;
    HRESULT hr = dev_->CreateTexture(w,
                                    h,
                                    1,
                                    D3DUSAGE_RENDERTARGET,
                                    D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT,
                                    &interTex_,
                                    nullptr);
    if (FAILED(hr) || !interTex_) {
      SafeRelease(interTex_);
      return false;
    }
    interW_ = w;
    interH_ = h;
    return true;
  }

  bool EnsureCompilerUnlocked() {
    if (fpCompile_) {
      return true;
    }
    static const wchar_t* kDlls[] = {
        L"d3dcompiler_47.dll",
        L"d3dcompiler_46.dll",
        L"d3dcompiler_45.dll",
        L"d3dcompiler_44.dll",
        L"d3dcompiler_43.dll",
        L"d3dcompiler_42.dll",
        L"d3dcompiler_41.dll",
    };
    for (const wchar_t* name : kDlls) {
      HMODULE m = LoadLibraryW(name);
      if (!m) {
        continue;
      }
      auto fn = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
      if (fn) {
        compilerMod_ = m;
        fpCompile_ = fn;
        return true;
      }
      FreeLibrary(m);
    }
    return false;
  }

  bool EnsureBicubicShadersUnlocked() {
    if (psCubicH_ && psCubicV_) {
      return true;
    }
    if (shadersTried_) {
      return false;
    }
    shadersTried_ = true;

    if (!EnsureCompilerUnlocked()) {
      return false;
    }

    // Catmull-Rom cubic (A=-0.5) with low instruction count (fits ps_2_0).
    static const char* kCubicHlslH =
      "float4 p : register(c0);\n" // x=srcW, y=srcH, z=invW, w=invH
      "sampler2D s0 : register(s0);\n"
      "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
      "  float x = uv.x * p.x - 0.5;\n"
      "  float ix = floor(x);\n"
      "  float t = x - ix;\n"
      "  float t2 = t * t;\n"
      "  float t3 = t2 * t;\n"
      "  float w0 = -0.5*t + 1.0*t2 - 0.5*t3;\n"
      "  float w1 = 1.0 - 2.5*t2 + 1.5*t3;\n"
      "  float w2 = 0.5*t + 2.0*t2 - 1.5*t3;\n"
      "  float w3 = -0.5*t2 + 0.5*t3;\n"
      "  float u0 = (ix - 1.0 + 0.5) * p.z;\n"
      "  float u1 = (ix + 0.0 + 0.5) * p.z;\n"
      "  float u2 = (ix + 1.0 + 0.5) * p.z;\n"
      "  float u3 = (ix + 2.0 + 0.5) * p.z;\n"
      "  float4 c = tex2D(s0, float2(u0, uv.y)) * w0 +\n"
      "            tex2D(s0, float2(u1, uv.y)) * w1 +\n"
      "            tex2D(s0, float2(u2, uv.y)) * w2 +\n"
      "            tex2D(s0, float2(u3, uv.y)) * w3;\n"
      "  return c;\n"
      "}\n";

    static const char* kCubicHlslV =
      "float4 p : register(c0);\n" // x=srcW, y=srcH, z=invW, w=invH
      "sampler2D s0 : register(s0);\n"
      "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
      "  float y = uv.y * p.y - 0.5;\n"
      "  float iy = floor(y);\n"
      "  float t = y - iy;\n"
      "  float t2 = t * t;\n"
      "  float t3 = t2 * t;\n"
      "  float w0 = -0.5*t + 1.0*t2 - 0.5*t3;\n"
      "  float w1 = 1.0 - 2.5*t2 + 1.5*t3;\n"
      "  float w2 = 0.5*t + 2.0*t2 - 1.5*t3;\n"
      "  float w3 = -0.5*t2 + 0.5*t3;\n"
      "  float v0 = (iy - 1.0 + 0.5) * p.w;\n"
      "  float v1 = (iy + 0.0 + 0.5) * p.w;\n"
      "  float v2 = (iy + 1.0 + 0.5) * p.w;\n"
      "  float v3 = (iy + 2.0 + 0.5) * p.w;\n"
      "  float4 c = tex2D(s0, float2(uv.x, v0)) * w0 +\n"
      "            tex2D(s0, float2(uv.x, v1)) * w1 +\n"
      "            tex2D(s0, float2(uv.x, v2)) * w2 +\n"
      "            tex2D(s0, float2(uv.x, v3)) * w3;\n"
      "  return c;\n"
      "}\n";

    ID3DBlob* codeH = nullptr;
    ID3DBlob* errH = nullptr;
    HRESULT hr = fpCompile_(kCubicHlslH, (SIZE_T)strlen(kCubicHlslH), "hklmwrap_ddraw_cubic_h", nullptr, nullptr,
                            "main", "ps_2_0", 0, 0, &codeH, &errH);
    if (FAILED(hr) || !codeH) {
      if (errH) {
        Tracef("bicubic shader compile (H) failed hr=0x%08lX: %s", (unsigned long)hr, (const char*)errH->GetBufferPointer());
      } else {
        Tracef("bicubic shader compile (H) failed hr=0x%08lX", (unsigned long)hr);
      }
      SafeRelease(errH);
      SafeRelease(codeH);
      return false;
    }
    SafeRelease(errH);

    ID3DBlob* codeV = nullptr;
    ID3DBlob* errV = nullptr;
    hr = fpCompile_(kCubicHlslV, (SIZE_T)strlen(kCubicHlslV), "hklmwrap_ddraw_cubic_v", nullptr, nullptr,
                    "main", "ps_2_0", 0, 0, &codeV, &errV);
    if (FAILED(hr) || !codeV) {
      if (errV) {
        Tracef("bicubic shader compile (V) failed hr=0x%08lX: %s", (unsigned long)hr, (const char*)errV->GetBufferPointer());
      } else {
        Tracef("bicubic shader compile (V) failed hr=0x%08lX", (unsigned long)hr);
      }
      SafeRelease(errV);
      SafeRelease(codeV);
      SafeRelease(codeH);
      return false;
    }
    SafeRelease(errV);

    hr = dev_->CreatePixelShader((const DWORD*)codeH->GetBufferPointer(), &psCubicH_);
    SafeRelease(codeH);
    if (FAILED(hr) || !psCubicH_) {
      SafeRelease(codeV);
      SafeRelease(psCubicH_);
      return false;
    }
    hr = dev_->CreatePixelShader((const DWORD*)codeV->GetBufferPointer(), &psCubicV_);
    SafeRelease(codeV);
    if (FAILED(hr) || !psCubicV_) {
      SafeRelease(psCubicH_);
      SafeRelease(psCubicV_);
      return false;
    }
    return true;
  }

  bool UploadSurfaceRectToSrcTextureUnlocked(LPDIRECTDRAWSURFACE7 srcSurf, const RECT& rc, UINT w, UINT h) {
    PixelFormatInfo srcFmt;
    if (!GetPixelFormatInfoFromSurface(srcSurf, &srcFmt)) {
      return false;
    }

    DDSURFACEDESC2 ssd{};
    ssd.dwSize = sizeof(ssd);
    // Avoid stalling on wrappers that keep surfaces on the GPU (common with dgVoodoo).
    // If we can't lock immediately, let caller fall back to point stretch.
    HRESULT hr = srcSurf->Lock(nullptr, &ssd, DDLOCK_DONOTWAIT | DDLOCK_READONLY, nullptr);
    if (FAILED(hr) || !ssd.lpSurface || ssd.lPitch <= 0) {
      if (SUCCEEDED(hr)) {
        srcSurf->Unlock(nullptr);
      }
      return false;
    }

    const uint8_t* sBase = (const uint8_t*)ssd.lpSurface;
    const int sPitch = (int)ssd.lPitch;
    const int bpp = srcFmt.bytesPerPixel;
    if (bpp != 2 && bpp != 4) {
      srcSurf->Unlock(nullptr);
      return false;
    }

    const size_t needed = (size_t)w * (size_t)h;
    if (staging_.size() < needed) {
      staging_.resize(needed);
    }

    // Fast paths for common formats.
    const bool isXrgb8888 =
        (bpp == 4) &&
        (srcFmt.rMask == 0x00FF0000) &&
        (srcFmt.gMask == 0x0000FF00) &&
        (srcFmt.bMask == 0x000000FF) &&
        (srcFmt.aMask == 0);
    const bool isArgb8888 =
        (bpp == 4) &&
        (srcFmt.rMask == 0x00FF0000) &&
        (srcFmt.gMask == 0x0000FF00) &&
        (srcFmt.bMask == 0x000000FF) &&
        (srcFmt.aMask == 0xFF000000);
    const bool isRgb565 =
        (bpp == 2) &&
        (srcFmt.rMask == 0xF800) &&
        (srcFmt.gMask == 0x07E0) &&
        (srcFmt.bMask == 0x001F) &&
        (srcFmt.aMask == 0);

    if (isArgb8888 || isXrgb8888) {
      for (UINT y = 0; y < h; y++) {
        const uint32_t* row = (const uint32_t*)(sBase + (ptrdiff_t)(rc.top + (LONG)y) * sPitch) + rc.left;
        uint32_t* out = &staging_[y * w];
        if (isArgb8888) {
          memcpy(out, row, (size_t)w * sizeof(uint32_t));
        } else {
          for (UINT x = 0; x < w; x++) {
            out[x] = row[x] | 0xFF000000u;
          }
        }
      }
    } else if (isRgb565) {
      for (UINT y = 0; y < h; y++) {
        const uint16_t* row = (const uint16_t*)(sBase + (ptrdiff_t)(rc.top + (LONG)y) * sPitch) + rc.left;
        uint32_t* out = &staging_[y * w];
        for (UINT x = 0; x < w; x++) {
          const uint16_t p16 = row[x];
          const uint32_t r5 = (uint32_t)((p16 >> 11) & 0x1F);
          const uint32_t g6 = (uint32_t)((p16 >> 5) & 0x3F);
          const uint32_t b5 = (uint32_t)(p16 & 0x1F);
          const uint32_t r8 = (r5 << 3) | (r5 >> 2);
          const uint32_t g8 = (g6 << 2) | (g6 >> 4);
          const uint32_t b8 = (b5 << 3) | (b5 >> 2);
          out[x] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
        }
      }
    } else {
      for (UINT y = 0; y < h; y++) {
        for (UINT x = 0; x < w; x++) {
          const int sx = (int)rc.left + (int)x;
          const int sy = (int)rc.top + (int)y;
          const uint32_t p = ReadPixel(sBase, sPitch, sx, sy, bpp);
          uint8_t r, g, b, a;
          UnpackRGBA(srcFmt, p, &r, &g, &b, &a);
          staging_[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
      }
    }

    srcSurf->Unlock(nullptr);

    D3DLOCKED_RECT lr{};
    hr = srcTex_->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr) || !lr.pBits || lr.Pitch <= 0) {
      if (SUCCEEDED(hr)) {
        srcTex_->UnlockRect(0);
      }
      return false;
    }
    const int dstPitch = (int)lr.Pitch;
    uint8_t* dst = (uint8_t*)lr.pBits;
    for (UINT y = 0; y < h; y++) {
      memcpy(dst + (size_t)y * (size_t)dstPitch, &staging_[y * w], (size_t)w * sizeof(uint32_t));
    }
    srcTex_->UnlockRect(0);
    return true;
  }

  void SetCommonDrawStateUnlocked(bool linear) {
    (void)dev_->SetRenderState(D3DRS_ZENABLE, FALSE);
    (void)dev_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    (void)dev_->SetRenderState(D3DRS_LIGHTING, FALSE);
    (void)dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    (void)dev_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    (void)dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    (void)dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    (void)dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    (void)dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    (void)dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    (void)dev_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    (void)dev_->SetSamplerState(0, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    (void)dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    (void)dev_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  }

  HRESULT RenderSinglePassUnlocked(IDirect3DTexture9* tex, UINT w, UINT h, bool linear) {
    if (!tex) {
      return E_INVALIDARG;
    }
    D3DVIEWPORT9 vp{};
    vp.X = 0;
    vp.Y = 0;
    vp.Width = w;
    vp.Height = h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    (void)dev_->SetViewport(&vp);

    SetCommonDrawStateUnlocked(linear);

    (void)dev_->SetPixelShader(nullptr);
    (void)dev_->SetTexture(0, tex);
    (void)dev_->SetFVF(kQuadFVF);

    const float fw = (float)w;
    const float fh = (float)h;
    QuadVtx v[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {fw - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, fh - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {fw - 0.5f, fh - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    HRESULT hr = dev_->BeginScene();
    if (FAILED(hr)) {
      return hr;
    }
    hr = dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVtx));
    (void)dev_->EndScene();
    return hr;
  }

  HRESULT RenderCubicPassUnlocked(IDirect3DTexture9* tex,
                                 IDirect3DPixelShader9* ps,
                                 UINT outW,
                                 UINT outH,
                                 UINT inW,
                                 UINT inH) {
    if (!tex || !ps || outW == 0 || outH == 0 || inW == 0 || inH == 0) {
      return E_INVALIDARG;
    }
    D3DVIEWPORT9 vp{};
    vp.X = 0;
    vp.Y = 0;
    vp.Width = outW;
    vp.Height = outH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    (void)dev_->SetViewport(&vp);

    // Point sampling; shader computes weights.
    SetCommonDrawStateUnlocked(/*linear=*/false);

    (void)dev_->SetTexture(0, tex);
    (void)dev_->SetPixelShader(ps);
    (void)dev_->SetFVF(kQuadFVF);

    const float params[4] = {
        (float)inW,
        (float)inH,
        1.0f / (float)inW,
        1.0f / (float)inH,
    };
    (void)dev_->SetPixelShaderConstantF(0, params, 1);

    const float fw = (float)outW;
    const float fh = (float)outH;
    QuadVtx v[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {fw - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, fh - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {fw - 0.5f, fh - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    HRESULT hr = dev_->BeginScene();
    if (FAILED(hr)) {
      return hr;
    }
    hr = dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVtx));
    (void)dev_->EndScene();
    return hr;
  }

  std::mutex mu_;
  HMODULE d3d9Mod_ = nullptr;
  Direct3DCreate9_t fpCreate9_ = nullptr;
  IDirect3D9* d3d_ = nullptr;
  IDirect3DDevice9* dev_ = nullptr;
  HWND hwnd_ = nullptr;
  UINT bbW_ = 0;
  UINT bbH_ = 0;

  IDirect3DTexture9* srcTex_ = nullptr;
  UINT srcW_ = 0;
  UINT srcH_ = 0;

  IDirect3DTexture9* interTex_ = nullptr;
  UINT interW_ = 0;
  UINT interH_ = 0;

  HMODULE compilerMod_ = nullptr;
  D3DCompile_t fpCompile_ = nullptr;
  bool shadersTried_ = false;

  IDirect3DPixelShader9* psCubicH_ = nullptr;
  IDirect3DPixelShader9* psCubicV_ = nullptr;

  std::vector<uint32_t> staging_;
};

static DDrawD3D9Scaler g_d3d9Scaler;
