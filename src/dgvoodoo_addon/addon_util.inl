
// Keep dgVoodoo resource tracking enabled; the backend relies on it to correctly manage
// resource states for swapchain/proxy textures when addons introduce transition barriers.

static bool IsTwinShimDebugEnabled() {
  // The wrapper only sets these when launched with --debug.
  wchar_t dummy[2] = {};
  DWORD n = GetEnvironmentVariableW(L"TWINSHIM_DEBUG_PIPE", dummy, (DWORD)(sizeof(dummy) / sizeof(dummy[0])));
  if (n) {
    return true;
  }
  n = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", dummy, (DWORD)(sizeof(dummy) / sizeof(dummy[0])));
  return n != 0;
}

static void Tracef(const char* fmt, ...) {
  if (!IsTwinShimDebugEnabled()) {
    return;
  }
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
  DWORD pipeLen = GetEnvironmentVariableW(L"TWINSHIM_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (!pipeLen || pipeLen >= (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    pipeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  }
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
  const twinshim::SurfaceScaleConfig& cfg = twinshim::GetSurfaceScaleConfig();
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

static twinshim::SurfaceScaleMethod GetScaleMethod() {
  return twinshim::GetSurfaceScaleConfig().method;
}

static bool IsTwoPassEnabledByEnv() {
  // Default ON (we want bilinear to be visible) but allow disabling for crash isolation.
  // Accept: 0/1, false/true.
  wchar_t buf[16] = {};
  DWORD n = GetEnvironmentVariableW(L"TWINSHIM_DGVOODOO_TWOPASS", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  if (!n || n >= (DWORD)(sizeof(buf) / sizeof(buf[0]))) {
    n = GetEnvironmentVariableW(L"HKLM_WRAPPER_DGVOODOO_TWOPASS", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  }
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

static D3D12_FILTER FilterForMethod(twinshim::SurfaceScaleMethod m) {
  switch (m) {
    case twinshim::SurfaceScaleMethod::kPoint:
      return D3D12_FILTER_MIN_MAG_MIP_POINT;
    case twinshim::SurfaceScaleMethod::kBilinear:
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case twinshim::SurfaceScaleMethod::kBicubic:
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case twinshim::SurfaceScaleMethod::kCatmullRom:
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case twinshim::SurfaceScaleMethod::kLanczos:
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case twinshim::SurfaceScaleMethod::kLanczos3:
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case twinshim::SurfaceScaleMethod::kPixelFast:
      return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    default:
      return D3D12_FILTER_MIN_MAG_MIP_POINT;
  }
}

