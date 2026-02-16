#include "shim/surface_scale_config.h"

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "Addon/AddonDefs.hpp"
#include "Addon/IAddonMainCallback.hpp"
#include "Addon/ID3D12RootObserver.hpp"

namespace {

// Keep dgVoodoo resource tracking enabled; the backend relies on it to correctly manage
// resource states for swapchain/proxy textures when addons introduce transition barriers.

static void Tracef(const char* fmt, ...) {
  if (!fmt || !*fmt) {
    return;
  }
  char buf[1024];
  buf[0] = '\0';

  int used = std::snprintf(buf, sizeof(buf), "[dgvoodoo-addon] ");
  if (used < 0) {
    used = 0;
    buf[0] = '\0';
  }

  va_list ap;
  va_start(ap, fmt);
  if ((size_t)used < sizeof(buf)) {
    (void)std::vsnprintf(buf + used, sizeof(buf) - (size_t)used, fmt, ap);
  }
  va_end(ap);

  buf[sizeof(buf) - 1] = '\0';
  size_t len = std::strlen(buf);
  if (len == 0 || buf[len - 1] != '\n') {
    if (len + 1 < sizeof(buf)) {
      buf[len] = '\n';
      buf[len + 1] = '\0';
    }
  }
  OutputDebugStringA(buf);

  // Mirror to the same debug pipe the wrapper/shim uses, if present.
  wchar_t pipeBuf[512] = {};
  DWORD pipeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (pipeLen && pipeLen < (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    pipeBuf[pipeLen] = L'\0';
    HANDLE h = CreateFileW(pipeBuf, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      (void)WriteFile(h, buf, (DWORD)lstrlenA(buf), &written, nullptr);
      CloseHandle(h);
    }
  }
}

static bool GetClientSize(HWND hwnd, int* outW, int* outH) {
  if (!outW || !outH) {
    return false;
  }
  *outW = 0;
  *outH = 0;
  RECT rc{};
  if (!hwnd || !GetClientRect(hwnd, &rc)) {
    return false;
  }
  const int w = (int)(rc.right - rc.left);
  const int h = (int)(rc.bottom - rc.top);
  if (w <= 0 || h <= 0) {
    return false;
  }
  *outW = w;
  *outH = h;
  return true;
}

static bool ResizeWindowClient(HWND hwnd, int clientW, int clientH) {
  if (!hwnd || clientW <= 0 || clientH <= 0) {
    return false;
  }
  const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
  RECT rc{0, 0, clientW, clientH};
  if (!AdjustWindowRectEx(&rc, (DWORD)style, FALSE, (DWORD)exStyle)) {
    return false;
  }
  const int outerW = rc.right - rc.left;
  const int outerH = rc.bottom - rc.top;
  if (outerW <= 0 || outerH <= 0) {
    return false;
  }

  // Try async SetWindowPos first (we can be called from a dgVoodoo worker thread).
  if (SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) != FALSE) {
    return true;
  }

  // Fallback: MoveWindow.
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr)) {
    return false;
  }
  return MoveWindow(hwnd, wr.left, wr.top, outerW, outerH, TRUE) != FALSE;
}

struct FindWindowCtx {
  DWORD pid = 0;
  HWND best = nullptr;
  long long bestArea = 0;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
  FindWindowCtx* ctx = reinterpret_cast<FindWindowCtx*>(lParam);
  if (!ctx) {
    return TRUE;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid) {
    return TRUE;
  }
  if (!IsWindowVisible(hwnd)) {
    return TRUE;
  }
  // Skip owned/tool windows where possible.
  if (GetWindow(hwnd, GW_OWNER) != nullptr) {
    return TRUE;
  }

  int cw = 0, ch = 0;
  if (!GetClientSize(hwnd, &cw, &ch)) {
    return TRUE;
  }
  const long long area = (long long)cw * (long long)ch;
  if (area > ctx->bestArea) {
    ctx->bestArea = area;
    ctx->best = hwnd;
  }
  return TRUE;
}

static HWND FindBestTopLevelWindowForCurrentProcess() {
  FindWindowCtx ctx;
  ctx.pid = GetCurrentProcessId();
  (void)EnumWindows(&EnumWindowsProc, (LPARAM)&ctx);
  return ctx.best;
}

static UINT CalcScaledUInt(UINT base, double factor) {
  if (base == 0) {
    return 0;
  }
  const double scaled = (double)base * factor;
  const double rounded = scaled + 0.5;
  if (rounded <= 0.0) {
    return 0;
  }
  if (rounded >= (double)0xFFFFFFFFu) {
    return 0xFFFFFFFFu;
  }
  return (UINT)rounded;
}

static bool IsScalingEnabled(double* outScale) {
  const hklmwrap::SurfaceScaleConfig& cfg = hklmwrap::GetSurfaceScaleConfig();
  if (outScale) {
    *outScale = cfg.factor;
  }
  return cfg.enabled && cfg.scaleValid && cfg.factor >= 1.1 && cfg.factor <= 100.0;
}

using D3DCompile_t = HRESULT(WINAPI*)(
  LPCVOID pSrcData,
  SIZE_T SrcDataSize,
  LPCSTR pSourceName,
  const D3D_SHADER_MACRO* pDefines,
  ID3DInclude* pInclude,
  LPCSTR pEntryPoint,
  LPCSTR pTarget,
  UINT Flags1,
  UINT Flags2,
  ID3DBlob** ppCode,
  ID3DBlob** ppErrorMsgs);

static hklmwrap::SurfaceScaleMethod GetScaleMethod() {
  return hklmwrap::GetSurfaceScaleConfig().method;
}

static bool IsTwoPassEnabledByEnv() {
  // Default ON (we want bilinear to be visible) but allow disabling for crash isolation.
  // Accept: 0/1, false/true.
  wchar_t buf[16] = {};
  DWORD n = GetEnvironmentVariableW(L"HKLM_WRAPPER_DGVOODOO_TWOPASS", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= (DWORD)(sizeof(buf) / sizeof(buf[0]))) {
    return true;
  }
  buf[n] = L'\0';
  if (buf[0] == L'0') {
    return false;
  }
  if (buf[0] == L'1') {
    return true;
  }
  if (buf[0] == L'f' || buf[0] == L'F') {
    return false;
  }
  if (buf[0] == L't' || buf[0] == L'T') {
    return true;
  }
  return true;
}

static void WideToUtf8BestEffort(const std::wstring& ws, char* out, size_t outSize) {
  if (!out || outSize == 0) {
    return;
  }
  out[0] = '\0';
  if (ws.empty()) {
    return;
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out, (int)(outSize - 1), nullptr, nullptr);
  if (n <= 0) {
    out[0] = '\0';
    return;
  }
  out[n] = '\0';
}

static D3D12_FILTER FilterForMethod(hklmwrap::SurfaceScaleMethod m) {
  switch (m) {
    case hklmwrap::SurfaceScaleMethod::kPoint:
      return D3D12_FILTER_MIN_MAG_MIP_POINT;
    case hklmwrap::SurfaceScaleMethod::kBilinear:
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case hklmwrap::SurfaceScaleMethod::kBicubic:
      // No native bicubic filter in fixed-function sampler; use linear as a safe fallback.
      // (A custom bicubic shader can be added later.)
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    default:
      return D3D12_FILTER_MIN_MAG_MIP_POINT;
  }
}

struct SwapchainState {
  UInt32 adapterID = 0;
  ID3D12Resource* outputTex = nullptr;
  UINT outputTexState = D3D12_RESOURCE_STATE_RENDER_TARGET;

  UInt32 outputSrvHandle = (UInt32)-1;
  UInt32 outputRtvHandle = (UInt32)-1;

  // CPU-only descriptor handles (dgVoodoo provides allocators for these).
  D3D12_CPU_DESCRIPTOR_HANDLE outputSrvCpu{};
  D3D12_CPU_DESCRIPTOR_HANDLE outputRtvCpu{};

  UINT w = 0;
  UINT h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

  // Native (pre-upscale) image size as reported by dgVoodoo. We keep the first meaningful
  // value to be able to upscale with filtering even if the swapchain presentation size grows.
  UINT nativeW = 0;
  UINT nativeH = 0;

  // Intermediate downsample target (native size) used to force a visible bilinear upscale when dgVoodoo
  // has already expanded the swapchain/image size to the presentation size.
  ID3D12Resource* nativeTex = nullptr;
  UINT nativeTexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  UInt32 nativeSrvHandle = (UInt32)-1;
  UInt32 nativeRtvHandle = (UInt32)-1;
  D3D12_CPU_DESCRIPTOR_HANDLE nativeSrvCpu{};
  D3D12_CPU_DESCRIPTOR_HANDLE nativeRtvCpu{};
};

struct AdapterState {
  UInt32 adapterID = 0;
  ID3D12Device* dev = nullptr; // not owned

  ID3D12RootSignature* rs = nullptr;
  ID3D12PipelineState* psoPoint = nullptr;
  ID3D12PipelineState* psoLinear = nullptr;

  // Keep shader blobs alive while referenced by dgVoodoo's pipeline cache.
  ID3DBlob* vs = nullptr;
  ID3DBlob* psPoint = nullptr;
  ID3DBlob* psLinear = nullptr;

  ID3D12Root::GraphicsPLDesc plDescPoint{};
  ID3D12Root::GraphicsPLDesc plDescLinear{};

  ID3D12Buffer* vb = nullptr;
  UInt32 vbPos = 0;

  uint32_t psoFailCount = 0;
  bool psoDisabled = false;

  DXGI_FORMAT psoRtvFormat = DXGI_FORMAT_UNKNOWN;

  ID3D12ResourceDescAllocator* srvAlloc = nullptr; // not owned
  ID3D12ResourceDescAllocator* rtvAlloc = nullptr; // not owned

  // Descriptor allocator handles for our static sampler-free design.
  // (We rely on a static sampler baked into the root signature.)
};

struct Vertex {
  float pX;
  float pY;
  float tU;
  float tV;
};

static constexpr UInt32 kVbVertexCap = 2048;

class D3D12Observer final : public dgVoodoo::ID3D12RootObserver {
public:
  bool Init(dgVoodoo::IAddonMainCallback* mainCb) {
    mainCb_ = mainCb;
    return true;
  }

  void Shutdown() {
    // Best-effort cleanup; actual releasing happens through swapchain/adapter callbacks.
    mainCb_ = nullptr;
    root_ = nullptr;
  }

  bool D3D12RootCreated(HMODULE /*hD3D12Dll*/, ID3D12Root* pD3D12Root) override {
    std::lock_guard<std::mutex> lock(mu_);
    root_ = pD3D12Root;
    Tracef("D3D12RootCreated root=%p", (void*)pD3D12Root);
    return true;
  }

  void D3D12RootReleased(const ID3D12Root* /*pD3D12Root*/) override {
    std::lock_guard<std::mutex> lock(mu_);
    Tracef("D3D12RootReleased");
    root_ = nullptr;
    adapters_.clear();
    swapchains_.clear();
  }

  bool D3D12BeginUsingAdapter(UInt32 adapterID) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!root_) {
      return true;
    }
    AdapterState& st = adapters_[adapterID];
    st.adapterID = adapterID;
    st.dev = root_->GetDevice(adapterID);
    st.srvAlloc = root_->GetCBV_SRV_UAV_DescAllocator(adapterID);
    st.rtvAlloc = root_->GetRTV_DescAllocator(adapterID);
    Tracef("BeginUsingAdapter id=%u dev=%p", (unsigned)adapterID, (void*)st.dev);
    return true;
  }

  void D3D12EndUsingAdapter(UInt32 adapterID) override {
    std::lock_guard<std::mutex> lock(mu_);
    Tracef("EndUsingAdapter id=%u", (unsigned)adapterID);
    auto it = adapters_.find(adapterID);
    if (it != adapters_.end()) {
      // NOTE: root signature / PSO are COM objects; safe to Release here.
      if (it->second.psoPoint) {
        it->second.psoPoint->Release();
        it->second.psoPoint = nullptr;
      }
      if (it->second.psoLinear) {
        it->second.psoLinear->Release();
        it->second.psoLinear = nullptr;
      }

      if (it->second.vb) {
        it->second.vb->Release();
        it->second.vb = nullptr;
        it->second.vbPos = 0;
      }
      if (it->second.rs) {
        if (root_) {
          root_->GPLRootSignatureReleased(it->second.adapterID, it->second.rs);
        }
        it->second.rs->Release();
        it->second.rs = nullptr;
      }

      if (it->second.vs) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.vs);
        }
        it->second.vs->Release();
        it->second.vs = nullptr;
      }
      if (it->second.psPoint) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psPoint);
        }
        it->second.psPoint->Release();
        it->second.psPoint = nullptr;
      }
      if (it->second.psLinear) {
        if (root_) {
          root_->GPLShaderReleased(it->second.adapterID, it->second.psLinear);
        }
        it->second.psLinear->Release();
        it->second.psLinear = nullptr;
      }

      adapters_.erase(it);
    }
  }

  bool D3D12CreateSwapchainHook(UInt32 /*adapterID*/, IDXGIFactory1* /*pDxgiFactory*/, IUnknown* /*pCommandQueue*/, const DXGI_SWAP_CHAIN_DESC& /*desc*/, IDXGISwapChain** /*ppSwapChain*/) override {
    // We do not override swapchain creation.
    return false;
  }

  void D3D12SwapchainCreated(UInt32 adapterID, ID3D12Swapchain* pSwapchain, const ID3D12Root::SwapchainData& swapchainData) override {
    (void)swapchainData;
    std::lock_guard<std::mutex> lock(mu_);
    Tracef("SwapchainCreated adapter=%u sc=%p img=%ldx%ld pres=%ldx%ld fmt=%u",
           (unsigned)adapterID,
           (void*)pSwapchain,
           (long)swapchainData.imageSize.cx,
           (long)swapchainData.imageSize.cy,
           (long)swapchainData.imagePresentationSize.cx,
           (long)swapchainData.imagePresentationSize.cy,
           (unsigned)swapchainData.format);

    SwapchainState st;
    st.adapterID = adapterID;
    st.w = (UINT)swapchainData.imagePresentationSize.cx;
    st.h = (UINT)swapchainData.imagePresentationSize.cy;
    st.fmt = swapchainData.format;

    const UINT iw = (UINT)swapchainData.imageSize.cx;
    const UINT ih = (UINT)swapchainData.imageSize.cy;
    if (iw > 1 && ih > 1) {
      st.nativeW = iw;
      st.nativeH = ih;
    } else if (st.w > 1 && st.h > 1) {
      st.nativeW = st.w;
      st.nativeH = st.h;
    }
    swapchains_[pSwapchain] = st;
  }

  void D3D12SwapchainChanged(UInt32 adapterID, ID3D12Swapchain* pSwapchain, const ID3D12Root::SwapchainData& swapchainData) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      Tracef(
          "SwapchainChanged adapter=%u sc=%p img=%ldx%ld pres=%ldx%ld maxOv=%ldx%ld fmt=%u",
             (unsigned)adapterID,
             (void*)pSwapchain,
             (long)swapchainData.imageSize.cx,
             (long)swapchainData.imageSize.cy,
             (long)swapchainData.imagePresentationSize.cx,
             (long)swapchainData.imagePresentationSize.cy,
             (long)swapchainData.maxOverriddenInputTextureSize.cx,
             (long)swapchainData.maxOverriddenInputTextureSize.cy,
             (unsigned)swapchainData.format);

      auto it = swapchains_.find(pSwapchain);
      if (it == swapchains_.end()) {
        return;
      }
      // Force re-create of our output resources on next present.
      ReleaseSwapchainOutputUnlocked(it->second);
      it->second.adapterID = adapterID;
      it->second.w = (UINT)swapchainData.imagePresentationSize.cx;
      it->second.h = (UINT)swapchainData.imagePresentationSize.cy;
      it->second.fmt = swapchainData.format;

      // Capture native size once (first meaningful value wins).
      if (it->second.nativeW == 0 || it->second.nativeH == 0) {
        const UINT iw = (UINT)swapchainData.imageSize.cx;
        const UINT ih = (UINT)swapchainData.imageSize.cy;
        if (iw > 1 && ih > 1) {
          it->second.nativeW = iw;
          it->second.nativeH = ih;
        } else if (it->second.w > 1 && it->second.h > 1) {
          it->second.nativeW = it->second.w;
          it->second.nativeH = it->second.h;
        }
      }
    }

    // Resize the host app window as soon as we learn the presentation size,
    // so the *first* PresentBegin can observe srcRect!=dstRect (allowing real
    // filtered scaling instead of a 1:1 copy).
    double scale = 1.0;
    if (IsScalingEnabled(&scale)) {
      MaybeResizeWindowOnce(scale);
    }
  }

  void D3D12SwapchainReleased(UInt32 /*adapterID*/, ID3D12Swapchain* pSwapchain) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = swapchains_.find(pSwapchain);
    if (it == swapchains_.end()) {
      return;
    }
    Tracef("SwapchainReleased sc=%p", (void*)pSwapchain);
    ReleaseSwapchainOutputUnlocked(it->second);
    ReleaseSwapchainNativeUnlocked(it->second);
    swapchains_.erase(it);
  }

  bool D3D12SwapchainPresentBegin(UInt32 adapterID, const PresentBeginContextInput& iCtx, PresentBeginContextOutput& oCtx) override {
    // Default: do nothing.
    oCtx.pOutputTexture = nullptr;
    oCtx.outputTexSRVCPUHandle.ptr = 0;
    oCtx.outputTextureExpectedState = (UINT)-1;

    double scale = 1.0;
    if (!IsScalingEnabled(&scale)) {
      return false;
    }

    {
      static std::atomic<int> callCount{0};
      const int n = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 10) {
        Tracef(
            "PresentBegin #%d sc=%p srcTex=%p srcState=%u srcRect=[%ld,%ld-%ld,%ld] dstTex=%p dstState=%u dstRect=[%ld,%ld-%ld,%ld]",
            n,
            (void*)iCtx.pSwapchain,
            (void*)iCtx.pSrcTexture,
            (unsigned)iCtx.srcTextureState,
            (long)iCtx.srcRect.left,
            (long)iCtx.srcRect.top,
            (long)iCtx.srcRect.right,
            (long)iCtx.srcRect.bottom,
            (void*)iCtx.drawingTarget.pDstTexture,
            (unsigned)iCtx.drawingTarget.dstTextureState,
            (long)iCtx.drawingTarget.dstRect.left,
            (long)iCtx.drawingTarget.dstRect.top,
            (long)iCtx.drawingTarget.dstRect.right,
            (long)iCtx.drawingTarget.dstRect.bottom);
      }
    }

    {
      static std::atomic<bool> loggedSrcState{false};
      bool expected = false;
      if (loggedSrcState.compare_exchange_strong(expected, true)) {
        Tracef("srcTextureState initial=%u", (unsigned)iCtx.srcTextureState);
      }
    }

    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        const hklmwrap::SurfaceScaleConfig& cfg = hklmwrap::GetSurfaceScaleConfig();
        char raw[256] = {};
        WideToUtf8BestEffort(cfg.methodRaw, raw, sizeof(raw));
        Tracef(
            "PresentBegin active (scale=%.3f method=%u methodSpecified=%d methodValid=%d raw='%s')",
            scale,
            (unsigned)cfg.method,
            cfg.methodSpecified ? 1 : 0,
            cfg.methodValid ? 1 : 0,
            raw);

        if (resizedHwnd_) {
          int cw = 0, ch = 0;
          if (GetClientSize(resizedHwnd_, &cw, &ch)) {
            Tracef("resized HWND %p current client=%dx%d", (void*)resizedHwnd_, cw, ch);
          } else {
            Tracef("resized HWND %p current client=<query failed>", (void*)resizedHwnd_);
          }
        }
      }
    }

    // If a resize was requested but it didn't stick (some games resize back), retry a limited number of times.
    if (resizedHwnd_ && desiredClientW_ > 0 && desiredClientH_ > 0) {
      int cw = 0, ch = 0;
      if (GetClientSize(resizedHwnd_, &cw, &ch)) {
        if ((cw != desiredClientW_ || ch != desiredClientH_) && resizeRetryCount_ < 120) {
          resizeRetryCount_++;
          const bool ok = ResizeWindowClient(resizedHwnd_, desiredClientW_, desiredClientH_);
          if (resizeRetryCount_ <= 3 || resizeRetryCount_ == 30 || resizeRetryCount_ == 120) {
            const DWORD gle = GetLastError();
            Tracef("resize retry #%u -> %dx%d (ok=%d gle=%lu)", (unsigned)resizeRetryCount_, desiredClientW_, desiredClientH_, ok ? 1 : 0, (unsigned long)gle);
          }
        }
      }
    }

    const hklmwrap::SurfaceScaleMethod method = GetScaleMethod();
    if (method == hklmwrap::SurfaceScaleMethod::kPoint) {
      // For point sampling, let dgVoodoo present normally.
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        const hklmwrap::SurfaceScaleConfig& cfg = hklmwrap::GetSurfaceScaleConfig();
        char raw[256] = {};
        WideToUtf8BestEffort(cfg.methodRaw, raw, sizeof(raw));
        Tracef("PresentBegin: method resolved to POINT; returning false (methodSpecified=%d methodValid=%d raw='%s')", cfg.methodSpecified ? 1 : 0, cfg.methodValid ? 1 : 0, raw);
      }
      return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (!root_) {
      Tracef("PresentBegin: root_ is null");
      return false;
    }

    // During resize, dgVoodoo may destroy/recreate swapchain resources shortly after this callback.
    // Force submitting our recorded GPU work for a short period so dgVoodoo can fence/wait safely.
    bool forceFlush = false;
    if (flushCountdown_ > 0) {
      flushCountdown_--;
      forceFlush = true;
    }

    // Keep dgVoodoo tracking enabled so it can correctly manage swapchain/proxy resource states.

    auto itSc = swapchains_.find(iCtx.pSwapchain);
    if (itSc == swapchains_.end()) {
      Tracef("PresentBegin: swapchain not tracked sc=%p", (void*)iCtx.pSwapchain);
      return false;
    }
    SwapchainState& sc = itSc->second;
    sc.adapterID = adapterID;

    // Safety: if dgVoodoo reports the source texture as COPY_DEST, it may still be under upload/copy.
    // Sampling from it (even with a barrier) can be unstable on some drivers.
    if ((iCtx.srcTextureState & D3D12_RESOURCE_STATE_COPY_DEST) != 0) {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: srcTextureState includes COPY_DEST (%u); skipping override", (unsigned)iCtx.srcTextureState);
      }
      return false;
    }

    // Fallback native-size capture: if dgVoodoo didn't provide a meaningful imageSize at swapchain creation,
    // treat the first observed srcRect size as the native image size.
    if ((sc.nativeW == 0 || sc.nativeH == 0)) {
      const LONG sw = (iCtx.srcRect.right > iCtx.srcRect.left) ? (iCtx.srcRect.right - iCtx.srcRect.left) : 0;
      const LONG sh = (iCtx.srcRect.bottom > iCtx.srcRect.top) ? (iCtx.srcRect.bottom - iCtx.srcRect.top) : 0;
      if (sw > 1 && sh > 1) {
        sc.nativeW = (UINT)sw;
        sc.nativeH = (UINT)sh;
      }
    }

    AdapterState* ad = nullptr;
    {
      auto itAd = adapters_.find(adapterID);
      if (itAd != adapters_.end()) {
        ad = &itAd->second;
      }
    }
    if (!ad || !ad->dev || !ad->srvAlloc || !ad->rtvAlloc) {
      Tracef("PresentBegin: adapter state unavailable id=%u (dev=%p srvAlloc=%p rtvAlloc=%p)", (unsigned)adapterID, ad ? (void*)ad->dev : nullptr, ad ? (void*)ad->srvAlloc : nullptr, ad ? (void*)ad->rtvAlloc : nullptr);
      return false;
    }

    // Pick a dgVoodoo proxy texture for output. These are swapchain-sized and have valid SRV/RTV handles.
    // Preferred fast-path: if dgVoodoo provides a drawing target (swapchain RT) then draw directly into it.
    // If we return that same dst texture as output, dgVoodoo can skip its own postprocess copy.
    const bool hasDrawingTarget = (iCtx.drawingTarget.pDstTexture != nullptr) && (iCtx.drawingTarget.rtvCPUHandle.ptr != 0);
    if (hasDrawingTarget) {
      ID3D12Resource* dstTex = iCtx.drawingTarget.pDstTexture;
      UINT dstStateBefore = iCtx.drawingTarget.dstTextureState;

      // Record a simple fullscreen draw into the provided drawing target.
      ID3D12GraphicsCommandListAuto* autoCl = root_->GetGraphicsCommandListAuto(adapterID);
      if (!autoCl) {
        Tracef("PresentBegin: no auto command list");
        return false;
      }
      ID3D12GraphicsCommandList* cl = autoCl->GetCommandListInterface();
      if (!cl) {
        Tracef("PresentBegin: no command list interface");
        return false;
      }

      // Ensure our dynamic vertex buffer exists.
      if (!ad->vb) {
        ad->vb = root_->CreateDynamicBuffer(adapterID, kVbVertexCap * (UInt32)sizeof(Vertex), ID3D12Root::DA_VertexBufferPageHeapAllocator);
        ad->vbPos = 0;
        if (!ad->vb) {
          Tracef("PresentBegin: CreateDynamicBuffer failed");
          return false;
        }
      }

      // Allocate a GPU-visible SRV entry from dgVoodoo's ring buffer and copy the incoming SRV into it.
      ID3D12ResourceDescRingBuffer* srvRing = root_->GetCBV_SRV_UAV_RingBuffer(adapterID);
      if (!srvRing) {
        Tracef("PresentBegin: no SRV ring buffer");
        return false;
      }
      ID3D12ResourceDescRingBuffer::AllocData rd{};
      if (!srvRing->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rd)) {
        Tracef("PresentBegin: SRV ring alloc failed");
        return false;
      }
      ad->dev->CopyDescriptorsSimple(1, rd.cpuDescHandle, iCtx.srvCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      const D3D12_FILTER filter = FilterForMethod(method);

      // Prefer the proxy texture RTV format if available; it reflects the actual RTV format used by dgVoodoo.
      DXGI_FORMAT rtvFormatToUse = sc.fmt;
      {
        ID3D12Root::SwapchainProxyTextureData tmp{};
        if (root_->GetProxyTexture(iCtx.pSwapchain, 0, &tmp)) {
          if (tmp.rtvFormat != DXGI_FORMAT_UNKNOWN) {
            rtvFormatToUse = tmp.rtvFormat;
          }
        }
      }

      {
        static std::atomic<bool> loggedRtv{false};
        bool expected = false;
        if (loggedRtv.compare_exchange_strong(expected, true)) {
          Tracef("pipeline RTV format: swapchainFmt=%u chosenFmt=%u", (unsigned)sc.fmt, (unsigned)rtvFormatToUse);
        }
      }

      // If dgVoodoo already expanded the source image to the presentation size, then our draw is 1:1 and
      // linear filtering won't be visible. In that case do a 2-pass filter:
      //   1) downsample to native size with point sampling
      //   2) upsample to destination with bilinear sampling
      const LONG srcRectW = (iCtx.srcRect.right > iCtx.srcRect.left) ? (iCtx.srcRect.right - iCtx.srcRect.left) : 0;
      const LONG srcRectH = (iCtx.srcRect.bottom > iCtx.srcRect.top) ? (iCtx.srcRect.bottom - iCtx.srcRect.top) : 0;
      const bool srcMatchesPres = (srcRectW > 0 && srcRectH > 0 && (UINT)srcRectW == sc.w && (UINT)srcRectH == sc.h);

      const bool wantTwoPass = (method == hklmwrap::SurfaceScaleMethod::kBilinear) &&
                               srcMatchesPres &&
                               (sc.nativeW > 0 && sc.nativeH > 0) &&
                               (sc.w > sc.nativeW + 1 || sc.h > sc.nativeH + 1) &&
                               IsTwoPassEnabledByEnv();

      {
        static std::atomic<int> logCount{0};
        const int n = logCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10) {
          Tracef(
              "present cfg: wantTwoPass=%d native=%ux%u pres=%ux%u srcRect=%ldx%ld",
              wantTwoPass ? 1 : 0,
              (unsigned)sc.nativeW,
              (unsigned)sc.nativeH,
              (unsigned)sc.w,
              (unsigned)sc.h,
              (long)srcRectW,
              (long)srcRectH);
        }
      }

      if (wantTwoPass) {
        static std::atomic<bool> loggedTwoPass{false};
        bool expectedTP = false;
        if (loggedTwoPass.compare_exchange_strong(expectedTP, true)) {
          Tracef("two-pass filter enabled: native=%ux%u pres=%ux%u", (unsigned)sc.nativeW, (unsigned)sc.nativeH, (unsigned)sc.w, (unsigned)sc.h);
        }
      }

      if (wantTwoPass) {
        if (!EnsureNativeResourcesUnlocked(*ad, sc)) {
          Tracef("PresentBegin: EnsureNativeResources failed (native=%ux%u fmt=%u)", (unsigned)sc.nativeW, (unsigned)sc.nativeH, (unsigned)sc.fmt);
          return false;
        }
      }

      if (!EnsurePipelinesUnlocked(*ad, rtvFormatToUse)) {
        Tracef("PresentBegin: EnsurePipelines failed (fmt=%u)", (unsigned)rtvFormatToUse);
        return false;
      }

      autoCl->AFlushLock();

      // Ensure the incoming src texture is in a shader-resource state before sampling.
      // Do NOT transition back: dgVoodoo's tracking/presenter expects to own the subsequent transitions.
      const UINT srcStateBefore = iCtx.srcTextureState;
      if (iCtx.pSrcTexture && (srcStateBefore & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0) {
        {
          static std::atomic<int> nLog{0};
          const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
          if (n <= 6) {
            Tracef("barrier: src %p %u->PSR", (void*)iCtx.pSrcTexture, (unsigned)srcStateBefore);
          }
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = iCtx.pSrcTexture;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)srcStateBefore;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
      }

      // Transition dst to RT if needed.
      if (dstStateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        {
          static std::atomic<int> nLog{0};
          const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
          if (n <= 6) {
            Tracef("barrier: dst %p %u->RT", (void*)dstTex, (unsigned)dstStateBefore);
          }
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dstTex;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)dstStateBefore;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
      }

      cl->SetGraphicsRootSignature(ad->rs);
      ID3D12PipelineState* psoOnePass = (method == hklmwrap::SurfaceScaleMethod::kBilinear) ? ad->psoLinear : ad->psoPoint;
      cl->SetPipelineState(psoOnePass);

      const LONG dstL = iCtx.drawingTarget.dstRect.left;
      const LONG dstT = iCtx.drawingTarget.dstRect.top;
      const LONG dstR = iCtx.drawingTarget.dstRect.right;
      const LONG dstB = iCtx.drawingTarget.dstRect.bottom;
      const LONG dstW = (dstR > dstL) ? (dstR - dstL) : (LONG)sc.w;
      const LONG dstH = (dstB > dstT) ? (dstB - dstT) : (LONG)sc.h;
      D3D12_VIEWPORT vp{};
      vp.TopLeftX = (float)dstL;
      vp.TopLeftY = (float)dstT;
      vp.Width = (float)dstW;
      vp.Height = (float)dstH;
      vp.MinDepth = 0.0f;
      vp.MaxDepth = 1.0f;
      D3D12_RECT sr{dstL, dstT, dstL + dstW, dstT + dstH};
      cl->RSSetViewports(1, &vp);
      cl->RSSetScissorRects(1, &sr);

      // Clear to black so letterbox areas remain black.
      const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      cl->ClearRenderTargetView(iCtx.drawingTarget.rtvCPUHandle, clear, 0, nullptr);
      cl->OMSetRenderTargets(1, &iCtx.drawingTarget.rtvCPUHandle, FALSE, nullptr);

      // Bind the ring-buffer heap + descriptor table.
      cl->SetDescriptorHeaps(1, &rd.pHeap);
      cl->SetGraphicsRootDescriptorTable(0, rd.gpuDescHandle);

      // Fill 4 vertices (triangle strip) from full srcRect (no UV clamp; clamping caused zoom/cropping).
      const D3D12_RESOURCE_DESC srcDesc = iCtx.pSrcTexture ? iCtx.pSrcTexture->GetDesc() : D3D12_RESOURCE_DESC{};
      const float srcW = (float)((srcDesc.Width > 0) ? srcDesc.Width : 1);
      const float srcH = (float)((srcDesc.Height > 0) ? srcDesc.Height : 1);
      const float tuLeft = (float)iCtx.srcRect.left / srcW;
      const float tvTop = (float)iCtx.srcRect.top / srcH;
      const float tuRight = (float)iCtx.srcRect.right / srcW;
      const float tvBottom = (float)iCtx.srcRect.bottom / srcH;

      const bool useNoOverwrite = ((ad->vbPos + 4) <= kVbVertexCap);
      const ID3D12Buffer::LockData lData = ad->vb->Lock(useNoOverwrite ? ID3D12Buffer::LT_NoOverwrite : ID3D12Buffer::LT_Discard, autoCl->AGetFence(), autoCl->GetFenceValue());
      if (!lData.ptr || lData.gpuAddress == 0) {
        Tracef("PresentBegin: vb lock failed");
        return false;
      }
      if (!useNoOverwrite) {
        ad->vbPos = 0;
      }

      volatile Vertex* v = ((Vertex*)lData.ptr) + ad->vbPos;
      v[0].pX = -1.0f;
      v[0].pY = 1.0f;
      v[0].tU = tuLeft;
      v[0].tV = tvTop;
      v[1].pX = -1.0f;
      v[1].pY = -1.0f;
      v[1].tU = tuLeft;
      v[1].tV = tvBottom;
      v[2].pX = 1.0f;
      v[2].pY = 1.0f;
      v[2].tU = tuRight;
      v[2].tV = tvTop;
      v[3].pX = 1.0f;
      v[3].pY = -1.0f;
      v[3].tU = tuRight;
      v[3].tV = tvBottom;

      const UInt64 vbGpu = lData.gpuAddress + (UInt64)ad->vbPos * (UInt64)sizeof(Vertex);
      ad->vb->Unlock();

      const D3D12_VERTEX_BUFFER_VIEW vbv{vbGpu, 4u * (UInt32)sizeof(Vertex), (UInt32)sizeof(Vertex)};

      if (!wantTwoPass) {
        cl->IASetVertexBuffers(0, 1, &vbv);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4, 1, 0, 0);
        ad->vbPos += 4;
      } else {
        // Pass 1: downsample src -> nativeTex with point sampling.
        // Allocate SRV descriptor for current src.
        ID3D12ResourceDescRingBuffer* srvRing2 = root_->GetCBV_SRV_UAV_RingBuffer(adapterID);
        ID3D12ResourceDescRingBuffer::AllocData rdSrc{};
        if (!srvRing2 || !srvRing2->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rdSrc)) {
          Tracef("PresentBegin: SRV ring alloc failed (pass1)");
          return false;
        }
        ad->dev->CopyDescriptorsSimple(1, rdSrc.cpuDescHandle, iCtx.srvCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Transition nativeTex to RT.
        if (sc.nativeTexState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("twopass: pass1 native->RT begin");
            }
          }
          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("barrier: native %p %u->RT", (void*)sc.nativeTex, (unsigned)sc.nativeTexState);
            }
          }
          D3D12_RESOURCE_BARRIER b{};
          b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b.Transition.pResource = sc.nativeTex;
          b.Transition.StateBefore = (D3D12_RESOURCE_STATES)sc.nativeTexState;
          b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
          b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &b);
          sc.nativeTexState = D3D12_RESOURCE_STATE_RENDER_TARGET;

          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("twopass: pass1 native->RT end");
            }
          }
        }

        // Set viewport to native size.
        D3D12_VIEWPORT vp1{};
        vp1.TopLeftX = 0.0f;
        vp1.TopLeftY = 0.0f;
        vp1.Width = (float)sc.nativeW;
        vp1.Height = (float)sc.nativeH;
        vp1.MinDepth = 0.0f;
        vp1.MaxDepth = 1.0f;
        D3D12_RECT sr1{0, 0, (LONG)sc.nativeW, (LONG)sc.nativeH};
        cl->RSSetViewports(1, &vp1);
        cl->RSSetScissorRects(1, &sr1);

        // Pass 1 uses point PSO.
        cl->SetGraphicsRootSignature(ad->rs);
        cl->SetPipelineState(ad->psoPoint);
        cl->OMSetRenderTargets(1, &sc.nativeRtvCpu, FALSE, nullptr);
        cl->SetDescriptorHeaps(1, &rdSrc.pHeap);
        cl->SetGraphicsRootDescriptorTable(0, rdSrc.gpuDescHandle);
        cl->IASetVertexBuffers(0, 1, &vbv);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4, 1, 0, 0);

        // Transition nativeTex to PS.
        {
          {
            static std::atomic<int> nLog{0};
            const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) {
              Tracef("barrier: native %p RT->PSR", (void*)sc.nativeTex);
            }
          }
          D3D12_RESOURCE_BARRIER b{};
          b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b.Transition.pResource = sc.nativeTex;
          b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
          b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &b);
          sc.nativeTexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // Pass 2 uses linear PSO (no pipeline rebuild here; avoids mid-frame release/recreate).
        cl->SetPipelineState(ad->psoLinear);

        // Restore dst viewport/scissor.
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sr);
        cl->OMSetRenderTargets(1, &iCtx.drawingTarget.rtvCPUHandle, FALSE, nullptr);

        ID3D12ResourceDescRingBuffer::AllocData rdNat{};
        if (!srvRing2->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rdNat)) {
          Tracef("PresentBegin: SRV ring alloc failed (pass2)");
          return false;
        }
        ad->dev->CopyDescriptorsSimple(1, rdNat.cpuDescHandle, sc.nativeSrvCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cl->SetDescriptorHeaps(1, &rdNat.pHeap);
        cl->SetGraphicsRootDescriptorTable(0, rdNat.gpuDescHandle);
        cl->IASetVertexBuffers(0, 1, &vbv);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4, 1, 0, 0);

        ad->vbPos += 4;
      }

      // Do NOT transition dst back: dgVoodoo can present directly from the swapchain texture when we return it.

      (void)autoCl->AFlushUnlock(forceFlush);

      oCtx.pOutputTexture = dstTex;
      oCtx.outputTexSRVCPUHandle.ptr = 0;
      oCtx.outputTextureExpectedState = (UINT)-1;

      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: drew into drawingTarget (filtered=%u)", (unsigned)filter);
      }
      return true;
    }

    ID3D12Root::SwapchainProxyTextureData proxy{};
    UInt32 proxyIdxChosen = (UInt32)-1;
    const UInt32 proxyCount = root_->GetMaxNumberOfProxyTextures(adapterID);
    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: proxyCount=%u", (unsigned)proxyCount);
      }
    }
    for (UInt32 idx = 0; idx < proxyCount; idx++) {
      ID3D12Root::SwapchainProxyTextureData tmp{};
      if (!root_->GetProxyTexture(iCtx.pSwapchain, idx, &tmp)) {
        continue;
      }
      if (!tmp.pTexture) {
        continue;
      }
      // Avoid writing into the current source texture if it happens to be a proxy.
      if (tmp.pTexture == iCtx.pSrcTexture) {
        continue;
      }
      proxy = tmp;
      proxyIdxChosen = idx;
      break;
    }
    if (proxyIdxChosen == (UInt32)-1 || !proxy.pTexture || proxy.srvHandle.ptr == 0 || proxy.rtvHandle.ptr == 0) {
      Tracef("PresentBegin: no suitable proxy texture available (count=%u)", (unsigned)proxyCount);
      return false;
    }

    // dgVoodoo proxy textures can have an RTV format different from the swapchain format (e.g. typeless backing + concrete RTV).
    // Using the wrong RTV format can cause pipeline creation to fail in the backend.
    const DXGI_FORMAT proxyRtvFormat = (proxy.rtvFormat != DXGI_FORMAT_UNKNOWN) ? proxy.rtvFormat : sc.fmt;

    {
      static std::atomic<bool> loggedProxyFmt{false};
      bool expected = false;
      if (loggedProxyFmt.compare_exchange_strong(expected, true)) {
        Tracef("proxy RTV format: swapchainFmt=%u proxyRtvFmt=%u chosenFmt=%u", (unsigned)sc.fmt, (unsigned)proxy.rtvFormat, (unsigned)proxyRtvFormat);
      }
    }

    const UINT proxyStateBefore = proxy.texState;

    const D3D12_FILTER filter = FilterForMethod(method);
    if (!EnsurePipelinesUnlocked(*ad, proxyRtvFormat)) {
      Tracef("PresentBegin: EnsurePipelines failed (fmt=%u)", (unsigned)proxyRtvFormat);
      return false;
    }

    // Record a simple fullscreen draw into our output texture.
    ID3D12GraphicsCommandListAuto* autoCl = root_->GetGraphicsCommandListAuto(adapterID);
    if (!autoCl) {
      return false;
    }
    ID3D12GraphicsCommandList* cl = autoCl->GetCommandListInterface();
    if (!cl) {
      return false;
    }

    // Allocate a GPU-visible SRV entry from dgVoodoo's ring buffer and copy the incoming SRV into it.
    ID3D12ResourceDescRingBuffer* srvRing = root_->GetCBV_SRV_UAV_RingBuffer(adapterID);
    if (!srvRing) {
      Tracef("PresentBegin: no SRV ring buffer");
      return false;
    }
    ID3D12ResourceDescRingBuffer::AllocData rd{};
    if (!srvRing->Alloc(1, autoCl->AGetFence(), autoCl->GetFenceValue(), rd)) {
      Tracef("PresentBegin: SRV ring alloc failed");
      return false;
    }
    ad->dev->CopyDescriptorsSimple(1, rd.cpuDescHandle, iCtx.srvCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    autoCl->AFlushLock();

    // Ensure the incoming src texture is in a shader-resource state before sampling.
    // Do NOT transition back: dgVoodoo's tracking/presenter expects to own the subsequent transitions.
    const UINT srcStateBefore = iCtx.srcTextureState;
    if (iCtx.pSrcTexture && (srcStateBefore & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0) {
      {
        static std::atomic<int> nLog{0};
        const int n = nLog.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 6) {
          Tracef("barrier: src %p %u->PSR", (void*)iCtx.pSrcTexture, (unsigned)srcStateBefore);
        }
      }
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = iCtx.pSrcTexture;
      b.Transition.StateBefore = (D3D12_RESOURCE_STATES)srcStateBefore;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
    }

    {
      static std::atomic<bool> logged{false};
      bool expected = false;
      if (logged.compare_exchange_strong(expected, true)) {
        Tracef("PresentBegin: using proxy idx=%u state=%u srcState=%u", (unsigned)proxyIdxChosen, (unsigned)proxy.texState, (unsigned)iCtx.srcTextureState);
      }
    }

    // Transition proxy output to RT.
    if (proxy.texState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = proxy.pTexture;
      b.Transition.StateBefore = (D3D12_RESOURCE_STATES)proxy.texState;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
      proxy.texState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Set pipeline.
    cl->SetGraphicsRootSignature(ad->rs);
    cl->SetPipelineState(method == hklmwrap::SurfaceScaleMethod::kBilinear ? ad->psoLinear : ad->psoPoint);

    // Viewport + scissor to destination rect (handles aspect-ratio letterboxing cases).
    const LONG dstL = iCtx.drawingTarget.dstRect.left;
    const LONG dstT = iCtx.drawingTarget.dstRect.top;
    const LONG dstR = iCtx.drawingTarget.dstRect.right;
    const LONG dstB = iCtx.drawingTarget.dstRect.bottom;
    const LONG dstW = (dstR > dstL) ? (dstR - dstL) : (LONG)sc.w;
    const LONG dstH = (dstB > dstT) ? (dstB - dstT) : (LONG)sc.h;

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = (float)dstL;
    vp.TopLeftY = (float)dstT;
    vp.Width = (float)dstW;
    vp.Height = (float)dstH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT sr{dstL, dstT, dstL + dstW, dstT + dstH};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);

    // Clear to black so areas outside dstRect remain black.
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cl->ClearRenderTargetView(proxy.rtvHandle, clear, 0, nullptr);

    // RTV.
    cl->OMSetRenderTargets(1, &proxy.rtvHandle, FALSE, nullptr);

    // Bind the ring-buffer heap + descriptor table.
    cl->SetDescriptorHeaps(1, &rd.pHeap);
    cl->SetGraphicsRootDescriptorTable(0, rd.gpuDescHandle);

    // Ensure our dynamic vertex buffer exists.
    if (!ad->vb) {
      ad->vb = root_->CreateDynamicBuffer(adapterID, kVbVertexCap * (UInt32)sizeof(Vertex), ID3D12Root::DA_VertexBufferPageHeapAllocator);
      ad->vbPos = 0;
      if (!ad->vb) {
        Tracef("PresentBegin: CreateDynamicBuffer failed");
        return false;
      }
    }

    // Fill 4 vertices (triangle strip) with the same native-size UV clamp logic as the drawingTarget path.
    const D3D12_RESOURCE_DESC srcDesc = iCtx.pSrcTexture ? iCtx.pSrcTexture->GetDesc() : D3D12_RESOURCE_DESC{};
    const float srcW = (float)((srcDesc.Width > 0) ? srcDesc.Width : 1);
    const float srcH = (float)((srcDesc.Height > 0) ? srcDesc.Height : 1);
    const LONG nativeR = (sc.nativeW > 0) ? (LONG)sc.nativeW : iCtx.srcRect.right;
    const LONG nativeB = (sc.nativeH > 0) ? (LONG)sc.nativeH : iCtx.srcRect.bottom;
    const LONG useR = (iCtx.srcRect.right < nativeR) ? iCtx.srcRect.right : nativeR;
    const LONG useB = (iCtx.srcRect.bottom < nativeB) ? iCtx.srcRect.bottom : nativeB;
    const float tuLeft = (float)iCtx.srcRect.left / srcW;
    const float tvTop = (float)iCtx.srcRect.top / srcH;
    const float tuRight = (float)useR / srcW;
    const float tvBottom = (float)useB / srcH;

    const bool useNoOverwrite = ((ad->vbPos + 4) <= kVbVertexCap);
    const ID3D12Buffer::LockData lData = ad->vb->Lock(useNoOverwrite ? ID3D12Buffer::LT_NoOverwrite : ID3D12Buffer::LT_Discard, autoCl->AGetFence(), autoCl->GetFenceValue());
    if (!lData.ptr || lData.gpuAddress == 0) {
      Tracef("PresentBegin: vb lock failed");
      return false;
    }
    if (!useNoOverwrite) {
      ad->vbPos = 0;
    }

    volatile Vertex* v = ((Vertex*)lData.ptr) + ad->vbPos;
    v[0].pX = -1.0f;
    v[0].pY = 1.0f;
    v[0].tU = tuLeft;
    v[0].tV = tvTop;
    v[1].pX = -1.0f;
    v[1].pY = -1.0f;
    v[1].tU = tuLeft;
    v[1].tV = tvBottom;
    v[2].pX = 1.0f;
    v[2].pY = 1.0f;
    v[2].tU = tuRight;
    v[2].tV = tvTop;
    v[3].pX = 1.0f;
    v[3].pY = -1.0f;
    v[3].tU = tuRight;
    v[3].tV = tvBottom;

    const UInt64 vbGpu = lData.gpuAddress + (UInt64)ad->vbPos * (UInt64)sizeof(Vertex);
    ad->vb->Unlock();

    const D3D12_VERTEX_BUFFER_VIEW vbv{vbGpu, 4u * (UInt32)sizeof(Vertex), (UInt32)sizeof(Vertex)};
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    cl->DrawInstanced(4, 1, 0, 0);
    ad->vbPos += 4;

    // Do NOT transition src back.

    // Transition proxy output to SRV state for presenter.
    {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = proxy.pTexture;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
      proxy.texState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    (void)autoCl->AFlushUnlock(forceFlush);

    // Output override.
    oCtx.pOutputTexture = proxy.pTexture;
    oCtx.outputTexSRVCPUHandle = proxy.srvHandle;
    oCtx.outputTextureExpectedState = (UINT)-1;
    return true;
  }

  void D3D12SwapchainPresentEnd(UInt32 /*adapterID*/, const PresentEndContextInput& /*iCtx*/) override {
    // Not used.
  }

private:
  void MaybeResizeWindowOnce(double factor) {
    if (factor <= 1.0) {
      return;
    }
    bool expected = false;
    if (!didResize_.compare_exchange_strong(expected, true)) {
      return;
    }

    HWND hwnd = FindBestTopLevelWindowForCurrentProcess();
    if (!hwnd) {
      Tracef("window resize skipped: no suitable top-level window found");
      return;
    }

    resizedHwnd_ = hwnd;
    {
      wchar_t cls[128] = {};
      wchar_t title[256] = {};
      (void)GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));
      (void)GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));
      char cls8[256] = {};
      char title8[512] = {};
      WideToUtf8BestEffort(std::wstring(cls), cls8, sizeof(cls8));
      WideToUtf8BestEffort(std::wstring(title), title8, sizeof(title8));
      Tracef("resize target hwnd=%p class='%s' title='%s'", (void*)hwnd, cls8, title8);
    }
    int cw = 0, ch = 0;
    if (!GetClientSize(hwnd, &cw, &ch)) {
      Tracef("window resize skipped: could not query client size");
      return;
    }
    const UINT dstW = CalcScaledUInt((UINT)cw, factor);
    const UINT dstH = CalcScaledUInt((UINT)ch, factor);

    desiredClientW_ = (int)dstW;
    desiredClientH_ = (int)dstH;
    resizeRetryCount_ = 0;
    flushCountdown_ = 120;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    Tracef("resize styles: style=0x%08lX ex=0x%08lX", (unsigned long)style, (unsigned long)exStyle);

    const bool ok = ResizeWindowClient(hwnd, (int)dstW, (int)dstH);
    const DWORD gle = GetLastError();
    int cw2 = 0, ch2 = 0;
    const bool ok2 = GetClientSize(hwnd, &cw2, &ch2);
    Tracef(
      "resize window client %dx%d -> %ux%u (scale=%.3f %s; after=%s %dx%d)",
      cw,
      ch,
      dstW,
      dstH,
      factor,
        ok ? "ok" : "failed",
      ok2 ? "ok" : "failed",
      cw2,
      ch2);
    if (!ok) {
      Tracef("resize initial failed gle=%lu", (unsigned long)gle);
    }
  }

  void ReleaseSwapchainOutputUnlocked(SwapchainState& sc) {
    if (!sc.outputTex) {
      return;
    }
    if (root_) {
      // Notify dgVoodoo tracking system because we used ResourceBarrier on this resource.
      (void)root_->RTResourceDestroyed(sc.outputTex, true);
    }
    sc.outputTex->Release();
    sc.outputTex = nullptr;
    sc.outputTexState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    sc.outputSrvCpu.ptr = 0;
    sc.outputRtvCpu.ptr = 0;

    // Free descriptor allocations.
    auto itAd = adapters_.find(sc.adapterID);
    if (itAd != adapters_.end() && itAd->second.srvAlloc && itAd->second.rtvAlloc) {
      if (sc.outputSrvHandle != (UInt32)-1) {
        itAd->second.srvAlloc->DeallocDescriptorGroup(sc.outputSrvHandle, 1, nullptr, 0);
      }
      if (sc.outputRtvHandle != (UInt32)-1) {
        itAd->second.rtvAlloc->DeallocDescriptorGroup(sc.outputRtvHandle, 1, nullptr, 0);
      }
    }
    sc.outputSrvHandle = (UInt32)-1;
    sc.outputRtvHandle = (UInt32)-1;
  }

  void ReleaseSwapchainNativeUnlocked(SwapchainState& sc) {
    if (!sc.nativeTex) {
      return;
    }
    if (root_) {
      (void)root_->RTResourceDestroyed(sc.nativeTex, true);
    }
    sc.nativeTex->Release();
    sc.nativeTex = nullptr;
    sc.nativeTexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    sc.nativeSrvCpu.ptr = 0;
    sc.nativeRtvCpu.ptr = 0;

    auto itAd = adapters_.find(sc.adapterID);
    if (itAd != adapters_.end() && itAd->second.srvAlloc && itAd->second.rtvAlloc) {
      if (sc.nativeSrvHandle != (UInt32)-1) {
        itAd->second.srvAlloc->DeallocDescriptorGroup(sc.nativeSrvHandle, 1, nullptr, 0);
      }
      if (sc.nativeRtvHandle != (UInt32)-1) {
        itAd->second.rtvAlloc->DeallocDescriptorGroup(sc.nativeRtvHandle, 1, nullptr, 0);
      }
    }
    sc.nativeSrvHandle = (UInt32)-1;
    sc.nativeRtvHandle = (UInt32)-1;
  }

  bool EnsureNativeResourcesUnlocked(AdapterState& ad, SwapchainState& sc) {
    if (sc.nativeW == 0 || sc.nativeH == 0 || sc.fmt == DXGI_FORMAT_UNKNOWN) {
      return false;
    }
    if (sc.nativeTex && sc.nativeSrvCpu.ptr && sc.nativeRtvCpu.ptr) {
      return true;
    }

    ReleaseSwapchainNativeUnlocked(sc);

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = sc.nativeW;
    rd.Height = sc.nativeH;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = sc.fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = sc.fmt;
    cv.Color[0] = 0.0f;
    cv.Color[1] = 0.0f;
    cv.Color[2] = 0.0f;
    cv.Color[3] = 1.0f;

    ID3D12Resource* tex = nullptr;
    HRESULT hr = ad.dev->CreateCommittedResource(
        &hp,
        D3D12_HEAP_FLAG_NONE,
        &rd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &cv,
        __uuidof(ID3D12Resource),
        (void**)&tex);
    if (FAILED(hr) || !tex) {
      Tracef("CreateCommittedResource(nativeTex) failed hr=0x%08lX", (unsigned long)hr);
      return false;
    }
    sc.nativeTex = tex;
    sc.nativeTexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    const UInt32 srvHandle = ad.srvAlloc->AllocDescriptorGroup(1);
    const UInt32 rtvHandle = ad.rtvAlloc->AllocDescriptorGroup(1);
    if (srvHandle == (UInt32)-1 || rtvHandle == (UInt32)-1) {
      Tracef("descriptor allocation failed (nativeTex)");
      ReleaseSwapchainNativeUnlocked(sc);
      return false;
    }
    sc.nativeSrvHandle = srvHandle;
    sc.nativeRtvHandle = rtvHandle;
    sc.nativeSrvCpu = ad.srvAlloc->GetCPUDescHandle(srvHandle, 0);
    sc.nativeRtvCpu = ad.rtvAlloc->GetCPUDescHandle(rtvHandle, 0);

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = sc.fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ad.dev->CreateShaderResourceView(sc.nativeTex, &sd, sc.nativeSrvCpu);

    D3D12_RENDER_TARGET_VIEW_DESC rdv{};
    rdv.Format = sc.fmt;
    rdv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rdv.Texture2D.MipSlice = 0;
    rdv.Texture2D.PlaneSlice = 0;
    ad.dev->CreateRenderTargetView(sc.nativeTex, &rdv, sc.nativeRtvCpu);

    return true;
  }

  bool EnsureOutputResourcesUnlocked(AdapterState& ad, SwapchainState& sc) {
    if (sc.w == 0 || sc.h == 0 || sc.fmt == DXGI_FORMAT_UNKNOWN) {
      return false;
    }
    if (sc.outputTex && sc.outputSrvCpu.ptr && sc.outputRtvCpu.ptr) {
      return true;
    }

    ReleaseSwapchainOutputUnlocked(sc);

    // Create output texture.
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment = 0;
    rd.Width = sc.w;
    rd.Height = sc.h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = sc.fmt;
    rd.SampleDesc.Count = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = sc.fmt;
    cv.Color[0] = 0.0f;
    cv.Color[1] = 0.0f;
    cv.Color[2] = 0.0f;
    cv.Color[3] = 1.0f;

    ID3D12Resource* tex = nullptr;
    HRESULT hr = ad.dev->CreateCommittedResource(
        &hp,
        D3D12_HEAP_FLAG_NONE,
        &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &cv,
        __uuidof(ID3D12Resource),
        (void**)&tex);
    if (FAILED(hr) || !tex) {
      Tracef("CreateCommittedResource failed hr=0x%08lX", (unsigned long)hr);
      return false;
    }
    sc.outputTex = tex;
    sc.outputTexState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // Allocate descriptors from dgVoodoo allocators (CPU-only heaps).
    const UInt32 srvHandle = ad.srvAlloc->AllocDescriptorGroup(1);
    const UInt32 rtvHandle = ad.rtvAlloc->AllocDescriptorGroup(1);
    if (srvHandle == (UInt32)-1 || rtvHandle == (UInt32)-1) {
      Tracef("descriptor allocation failed");
      ReleaseSwapchainOutputUnlocked(sc);
      return false;
    }
    sc.outputSrvHandle = srvHandle;
    sc.outputRtvHandle = rtvHandle;
    sc.outputSrvCpu = ad.srvAlloc->GetCPUDescHandle(srvHandle, 0);
    sc.outputRtvCpu = ad.rtvAlloc->GetCPUDescHandle(rtvHandle, 0);

    // Create SRV/RTV.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = sc.fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ad.dev->CreateShaderResourceView(sc.outputTex, &sd, sc.outputSrvCpu);

    D3D12_RENDER_TARGET_VIEW_DESC rdv{};
    rdv.Format = sc.fmt;
    rdv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rdv.Texture2D.MipSlice = 0;
    rdv.Texture2D.PlaneSlice = 0;
    ad.dev->CreateRenderTargetView(sc.outputTex, &rdv, sc.outputRtvCpu);

    return true;
  }

  static bool EnsureD3DCompilerLoaded(D3DCompile_t* outFn) {
    if (!outFn) {
      return false;
    }
    *outFn = nullptr;
    static std::atomic<HMODULE> g_mod{nullptr};
    static std::atomic<D3DCompile_t> g_fn{nullptr};
    if (auto fn = g_fn.load(std::memory_order_acquire)) {
      *outFn = fn;
      return true;
    }
    static const wchar_t* kDlls[] = {
        L"d3dcompiler_47.dll",
        L"d3dcompiler_46.dll",
        L"d3dcompiler_45.dll",
    };
    for (const wchar_t* name : kDlls) {
      HMODULE m = LoadLibraryW(name);
      if (!m) {
        continue;
      }
      auto fn = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
      if (fn) {
        g_mod.store(m, std::memory_order_release);
        g_fn.store(fn, std::memory_order_release);
        *outFn = fn;
        return true;
      }
      FreeLibrary(m);
    }
    return false;
  }

  static bool CompileHlsl(const char* src, const char* entry, const char* target, ID3DBlob** outBlob) {
    if (!src || !entry || !target || !outBlob) {
      return false;
    }
    *outBlob = nullptr;
    D3DCompile_t fp = nullptr;
    if (!EnsureD3DCompilerLoaded(&fp) || !fp) {
      return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const UINT flags1 = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = fp(src, std::strlen(src), "hklm_dgvoodoo_addon", nullptr, nullptr, entry, target, flags1, 0, &code, &err);
    if (FAILED(hr) || !code) {
      if (err) {
        Tracef("shader compile failed (%s/%s): %s", entry, target, (const char*)err->GetBufferPointer());
        err->Release();
      } else {
        Tracef("shader compile failed (%s/%s) hr=0x%08lX", entry, target, (unsigned long)hr);
      }
      if (code) {
        code->Release();
      }
      return false;
    }
    if (err) {
      err->Release();
    }
    *outBlob = code;
    return true;
  }

  bool EnsurePipelinesUnlocked(AdapterState& ad, DXGI_FORMAT rtvFormat) {
    if (ad.psoDisabled) {
      return false;
    }
    if (ad.rs && ad.psoPoint && ad.psoLinear && ad.psoRtvFormat == rtvFormat) {
      return true;
    }

    if (ad.psoPoint) {
      ad.psoPoint->Release();
      ad.psoPoint = nullptr;
    }
    if (ad.psoLinear) {
      ad.psoLinear->Release();
      ad.psoLinear = nullptr;
    }

    // Ensure we have shader blobs; dgVoodoo's pipeline cache uses ID3DBlob pointers.
    static const char* kHlsl =
        "Texture2D tex0 : register(t0, space1);\n"
        "SamplerState sampPoint : register(s0);\n"
        "SamplerState sampLinear : register(s1);\n"
        "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VS(VSIn i) { VSOut o; o.pos=float4(i.pos,0.0,1.0); o.uv=i.uv; return o; }\n"
        "float4 PSPoint(VSOut i) : SV_Target { return tex0.Sample(sampPoint, i.uv); }\n"
        "float4 PSLinear(VSOut i) : SV_Target { return tex0.Sample(sampLinear, i.uv); }\n";

    if (!ad.vs || !ad.psPoint || !ad.psLinear) {
      if (ad.vs) {
        ad.vs->Release();
        ad.vs = nullptr;
      }
      if (ad.psPoint) {
        ad.psPoint->Release();
        ad.psPoint = nullptr;
      }
      if (ad.psLinear) {
        ad.psLinear->Release();
        ad.psLinear = nullptr;
      }

      // Compile via D3DCompiler then clone into dgVoodoo-created blobs.
      // (The SDK sample uses ID3D12Root::CreateD3DBlob; using it here improves compatibility.)
      ID3DBlob* tmpVs = nullptr;
      ID3DBlob* tmpPsPoint = nullptr;
      ID3DBlob* tmpPsLinear = nullptr;
      if (!CompileHlsl(kHlsl, "VS", "vs_5_1", &tmpVs) || !CompileHlsl(kHlsl, "PSPoint", "ps_5_1", &tmpPsPoint) ||
          !CompileHlsl(kHlsl, "PSLinear", "ps_5_1", &tmpPsLinear)) {
        if (tmpVs) {
          tmpVs->Release();
        }
        if (tmpPsPoint) {
          tmpPsPoint->Release();
        }
        if (tmpPsLinear) {
          tmpPsLinear->Release();
        }
        Tracef("shader compile unavailable (d3dcompiler missing?)");
        return false;
      }

      ad.vs = root_->CreateD3DBlob((UIntPtr)tmpVs->GetBufferSize(), tmpVs->GetBufferPointer());
      ad.psPoint = root_->CreateD3DBlob((UIntPtr)tmpPsPoint->GetBufferSize(), tmpPsPoint->GetBufferPointer());
      ad.psLinear = root_->CreateD3DBlob((UIntPtr)tmpPsLinear->GetBufferSize(), tmpPsLinear->GetBufferPointer());

      tmpVs->Release();
      tmpPsPoint->Release();
      tmpPsLinear->Release();

      if (!ad.vs || !ad.psPoint || !ad.psLinear) {
        if (ad.vs) {
          ad.vs->Release();
          ad.vs = nullptr;
        }
        if (ad.psPoint) {
          ad.psPoint->Release();
          ad.psPoint = nullptr;
        }
        if (ad.psLinear) {
          ad.psLinear->Release();
          ad.psLinear = nullptr;
        }
        Tracef("CreateD3DBlob failed for compiled shaders");
        return false;
      }
    }

    // Root signature: descriptor table (t0, space1) + 2 static samplers (s0 point, s1 linear).
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 1;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[1]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samps[2]{};
    samps[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samps[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].MipLODBias = 0.0f;
    samps[0].MaxAnisotropy = 1;
    samps[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samps[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samps[0].MinLOD = 0.0f;
    samps[0].MaxLOD = D3D12_FLOAT32_MAX;
    samps[0].ShaderRegister = 0;
    samps[0].RegisterSpace = 0;
    samps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samps[1] = samps[0];
    samps[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samps[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = samps;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
          D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
          D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
          D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    if (!ad.rs) {
      // Serialize+create root signature via dgVoodoo helper (more compatible with dgVoodoo's backend).
      ID3DBlob* rsErr = nullptr;
      ad.rs = root_->SerializeAndCreateRootSignature(ad.adapterID, D3D_ROOT_SIGNATURE_VERSION_1, &rsd, &rsErr);
      if (!ad.rs) {
        if (rsErr) {
          Tracef("SerializeAndCreateRootSignature failed: %s", (const char*)rsErr->GetBufferPointer());
          rsErr->Release();
        } else {
          Tracef("SerializeAndCreateRootSignature failed (no error blob)");
        }
        return false;
      }
      if (rsErr) {
        rsErr->Release();
        rsErr = nullptr;
      }
    }

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; i++) {
      auto& rt = blend.RenderTarget[i];
      rt.BlendEnable = FALSE;
      rt.LogicOpEnable = FALSE;
      rt.SrcBlend = D3D12_BLEND_ONE;
      rt.DestBlend = D3D12_BLEND_ZERO;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      rt.SrcBlendAlpha = D3D12_BLEND_ONE;
      rt.DestBlendAlpha = D3D12_BLEND_ZERO;
      rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      rt.LogicOp = D3D12_LOGIC_OP_NOOP;
      rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rast.DepthClipEnable = TRUE;
    rast.MultisampleEnable = FALSE;
    rast.AntialiasedLineEnable = FALSE;
    rast.ForcedSampleCount = 0;
    rast.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.StencilEnable = FALSE;
    ds.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    ds.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.BackFace = ds.FrontFace;

    // Build a cache-friendly pipeline description using dgVoodoo's pipeline cache helpers.
    // (Direct CreateGraphicsPipelineState often fails under dgVoodoo's D3D12 backend with E_INVALIDARG.)
    ID3D12Root::GraphicsPLDesc pl{};
    std::memset(&pl, 0, sizeof(pl));
    pl.pRootSignature = ad.rs;
    pl.pVS = ad.vs;
    // We'll fill PS per variant below.
    pl.pPS = nullptr;
    pl.pDS = nullptr;
    pl.pHS = nullptr;
    pl.pGS = nullptr;
    pl.pStreamOutput = nullptr;
    pl.pBlendState = root_->PLCacheGetBlend4Desc(ad.adapterID, blend);
    pl.SampleMask = 0xFFFFFFFFu;
    pl.pRasterizerState = root_->PLCacheGetRasterizerDesc(ad.adapterID, rast);
    pl.pDepthStencilState = root_->PLCacheGetDepthStencilDesc(ad.adapterID, ds);
    // Input layout for our dynamic quad vertex buffer.
    static const D3D12_INPUT_ELEMENT_DESC kIlElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    pl.pInputLayout = root_->PLCacheGetInputLayoutDesc(ad.adapterID, 2, kIlElems);
    pl.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pl.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pl.NumRenderTargets = 1;
    for (int i = 0; i < 8; i++) {
      pl.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    }
    pl.RTVFormats[0] = rtvFormat;
    pl.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pl.SampleDesc = {1, 0};
    pl.NodeMask = 0;
    pl.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Point PSO
    pl.pPS = ad.psPoint;
    ad.plDescPoint = pl;
    ad.psoPoint = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescPoint);

    // Linear PSO
    pl.pPS = ad.psLinear;
    ad.plDescLinear = pl;
    ad.psoLinear = root_->PLCacheGetGraphicsPipeline(ad.adapterID, ad.plDescLinear);

    if (!ad.psoPoint || !ad.psoLinear) {
      {
        static std::atomic<bool> loggedOnce{false};
        bool expected = false;
        if (loggedOnce.compare_exchange_strong(expected, true)) {
          const size_t vsSz = pl.pVS ? (size_t)pl.pVS->GetBufferSize() : 0;
          const size_t ps0Sz = ad.psPoint ? (size_t)ad.psPoint->GetBufferSize() : 0;
          const size_t ps1Sz = ad.psLinear ? (size_t)ad.psLinear->GetBufferSize() : 0;
          Tracef(
              "PSO cache failure detail: rtvFmt=%u rs=%p vs=%p(v=%zu) psPoint=%p(v=%zu) psLin=%p(v=%zu) blend=%p rast=%p ds=%p il=%p topo=%u numRT=%u samp=(%u,%u)",
              (unsigned)rtvFormat,
              (void*)pl.pRootSignature,
              (void*)pl.pVS,
              vsSz,
              (void*)ad.psPoint,
              ps0Sz,
              (void*)ad.psLinear,
              ps1Sz,
              (void*)pl.pBlendState,
              (void*)pl.pRasterizerState,
              (void*)pl.pDepthStencilState,
              (void*)pl.pInputLayout,
              (unsigned)pl.PrimitiveTopologyType,
              (unsigned)pl.NumRenderTargets,
              (unsigned)pl.SampleDesc.Count,
              (unsigned)pl.SampleDesc.Quality);
        }
      }
      // Best-effort diagnostics: if any descriptor pointer is null, this is likely the root cause.
      if (!pl.pRootSignature || !pl.pVS || !ad.psPoint || !ad.psLinear || !pl.pBlendState || !pl.pRasterizerState || !pl.pDepthStencilState) {
        Tracef(
            "pipeline desc has nulls (rs=%p vs=%p psPoint=%p psLin=%p blend=%p rast=%p ds=%p il=%p)",
            (void*)pl.pRootSignature,
            (void*)pl.pVS,
            (void*)ad.psPoint,
            (void*)ad.psLinear,
            (void*)pl.pBlendState,
            (void*)pl.pRasterizerState,
            (void*)pl.pDepthStencilState,
            (void*)pl.pInputLayout);
      }
      ad.psoFailCount++;
      const uint32_t nFail = ad.psoFailCount;
      if (nFail <= 5 || (nFail % 120) == 0) {
        Tracef(
            "PLCacheGetGraphicsPipeline failed (failCount=%lu rtvFmt=%u)",
            (unsigned long)nFail,
            (unsigned)rtvFormat);
      }
      if (nFail >= 10) {
        ad.psoDisabled = true;
        Tracef("disabling filtered scaling: PSO creation repeatedly failed");
      }
      return false;
    }

    ad.psoRtvFormat = rtvFormat;
    return true;
  }

private:
  dgVoodoo::IAddonMainCallback* mainCb_ = nullptr; // not owned
  ID3D12Root* root_ = nullptr; // not owned

  std::mutex mu_;
  std::unordered_map<UInt32, AdapterState> adapters_;
  std::unordered_map<ID3D12Swapchain*, SwapchainState> swapchains_;

  std::atomic<bool> didResize_{false};
  HWND resizedHwnd_ = nullptr;

  int desiredClientW_ = 0;
  int desiredClientH_ = 0;
  uint32_t resizeRetryCount_ = 0;
  uint32_t flushCountdown_ = 0;

  // No global D3D12 objects; state is per-adapter/per-swapchain.
};

static D3D12Observer g_observer;
static dgVoodoo::IAddonMainCallback* g_main = nullptr;

static const char* kAddonBuildId = "hklm-wrapper SampleAddon (rev=ringbuf-11-dualpso) " __DATE__ " " __TIME__;

static bool AddonInitCommon(dgVoodoo::IAddonMainCallback* pAddonMain) {
  g_main = pAddonMain;
  Tracef("AddOnInit/AddOnInit called main=%p (%s)", (void*)pAddonMain, kAddonBuildId);
  if (!pAddonMain) {
    return false;
  }
  if (!g_observer.Init(pAddonMain)) {
    return false;
  }
  const bool ok = pAddonMain->RegisterForCallback(IID_D3D12RootObserver, &g_observer);
  Tracef("RegisterForCallback(IID_D3D12RootObserver) -> %d", ok ? 1 : 0);
  return ok;
}

static void AddonExitCommon() {
  Tracef("AddOnExit/AddOnExit called (%s)", kAddonBuildId);
  if (g_main) {
    g_main->UnregisterForCallback(IID_D3D12RootObserver, &g_observer);
  }
  g_observer.Shutdown();
  g_main = nullptr;
}

} // namespace

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
