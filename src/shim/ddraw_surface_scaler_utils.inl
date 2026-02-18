static constexpr double kMinScale = 1.1;
static constexpr double kMaxScale = 100.0;

static std::atomic<bool> g_active{false};

static std::atomic<bool> g_stopInitThread{false};
static HANDLE g_initThread = nullptr;

static std::atomic<bool> g_seenDDraw{false};
  static std::atomic<bool> g_loggedFirstCreateSurface{false};
  static std::atomic<uint32_t> g_flipCalls{0};
  static std::atomic<uint32_t> g_bltCalls{0};
  static std::atomic<uint32_t> g_bltFastCalls{0};
  static std::atomic<bool> g_loggedScaleViaFlip{false};
  static std::atomic<bool> g_loggedScaleViaBlt{false};
  static std::atomic<bool> g_loggedFilteredFallback{false};


static std::mutex g_stateMutex;
static HWND g_hwnd = nullptr;
static DWORD g_coopFlags = 0;
static bool g_resizedOnce = false;

static void Tracef(const char* fmt, ...);

// 0=unknown, 1=system ddraw, 2=wrapper ddraw (dgVoodoo/etc)
static std::atomic<int> g_ddrawModuleKind{0};

static std::wstring ToLowerCopy(const std::wstring& s) {
  std::wstring out = s;
  for (wchar_t& ch : out) {
    ch = (wchar_t)towlower(ch);
  }
  return out;
}

static bool IsTwinShimDebugEnabled() {
  wchar_t dummy[2] = {};
  DWORD n = GetEnvironmentVariableW(L"TWINSHIM_DEBUG_PIPE", dummy, (DWORD)(sizeof(dummy) / sizeof(dummy[0])));
  if (n) {
    return true;
  }
  n = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", dummy, (DWORD)(sizeof(dummy) / sizeof(dummy[0])));
  return n != 0;
}

static bool IsLikelyWrapperDDrawDll() {
  const int cached = g_ddrawModuleKind.load(std::memory_order_acquire);
  if (cached == 1) {
    return false;
  }
  if (cached == 2) {
    return true;
  }

  HMODULE h = GetModuleHandleW(L"ddraw.dll");
  if (!h) {
    return false;
  }

  wchar_t modPathBuf[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(h, modPathBuf, (DWORD)(sizeof(modPathBuf) / sizeof(modPathBuf[0])));
  if (!n || n >= (DWORD)(sizeof(modPathBuf) / sizeof(modPathBuf[0]))) {
    return false;
  }
  std::wstring modPath = ToLowerCopy(std::wstring(modPathBuf));

  wchar_t sysDirBuf[MAX_PATH] = {};
  UINT sn = GetSystemDirectoryW(sysDirBuf, (UINT)(sizeof(sysDirBuf) / sizeof(sysDirBuf[0])));
  std::wstring sysDir = ToLowerCopy(std::wstring(sysDirBuf, sysDirBuf + (sn ? sn : 0)));
  if (!sysDir.empty() && sysDir.back() != L'\\') {
    sysDir.push_back(L'\\');
  }

  bool isSystem = false;
  if (!sysDir.empty()) {
    // System32\ddraw.dll
    const std::wstring sysDdraw = sysDir + L"ddraw.dll";
    if (modPath == sysDdraw) {
      isSystem = true;
    }
  }

  // If it's not exactly the system DLL (common for app-local wrappers), treat as wrapper.
  const int kind = isSystem ? 1 : 2;
  g_ddrawModuleKind.store(kind, std::memory_order_release);

  static std::atomic<bool> logged{false};
  bool expected = false;
  if (logged.compare_exchange_strong(expected, true)) {
    Tracef("ddraw.dll path: %ls (%s)", modPathBuf, isSystem ? "system" : "wrapper");
  }

  return !isSystem;
}

static LPDIRECTDRAWSURFACE7 g_primary = nullptr;
static LPDIRECTDRAWSURFACE7 g_cachedBackbuffer = nullptr;
static DWORD g_cachedBackW = 0;
static DWORD g_cachedBackH = 0;

template <typename T>
static void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

static void* GetVtableEntry(void* obj, size_t index) {
  if (!obj) {
    return nullptr;
  }
  void** vtbl = *reinterpret_cast<void***>(obj);
  if (!vtbl) {
    return nullptr;
  }
  return vtbl[index];
}

static void TraceWrite(const char* text) {
  if (!IsTwinShimDebugEnabled()) {
    return;
  }
  if (!text || !*text) {
    return;
  }
  OutputDebugStringA(text);

  wchar_t pipeBuf[512] = {};
  DWORD pipeLen = GetEnvironmentVariableW(L"TWINSHIM_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (!pipeLen || pipeLen >= (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    pipeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  }
  if (!pipeLen || pipeLen >= (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    return;
  }
  pipeBuf[pipeLen] = L'\0';

  HANDLE h = CreateFileW(pipeBuf, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(h, text, (DWORD)lstrlenA(text), &written, nullptr);
  CloseHandle(h);
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
  int used = std::snprintf(buf, sizeof(buf), "[shim:ddraw] ");
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
  TraceWrite(buf);
}

static bool IsScalingEnabled() {
  const SurfaceScaleConfig& cfg = GetSurfaceScaleConfig();
  return cfg.enabled && cfg.scaleValid && cfg.factor >= kMinScale && cfg.factor <= kMaxScale;
}

static int CalcScaledInt(int base, double factor) {
  if (base <= 0) {
    return 0;
  }
  const double scaled = (double)base * factor;
  const double rounded = scaled + 0.5;
  if (rounded <= 0.0) {
    return 0;
  }
  if (rounded > (double)INT32_MAX) {
    return INT32_MAX;
  }
  return (int)rounded;
}

static bool GetClientSize(HWND hwnd, int* outW, int* outH) {
  if (!outW || !outH) {
    return false;
  }
  *outW = 0;
  *outH = 0;
  if (!hwnd) {
    return false;
  }
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) {
    return false;
  }
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  *outW = w;
  *outH = h;
  return true;
}

static bool GetClientRectInScreen(HWND hwnd, RECT* outRc) {
  if (!outRc) {
    return false;
  }
  *outRc = RECT{};
  if (!hwnd) {
    return false;
  }
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) {
    return false;
  }
  POINT pt{rc.left, rc.top};
  if (!ClientToScreen(hwnd, &pt)) {
    return false;
  }
  const int w = (int)(rc.right - rc.left);
  const int h = (int)(rc.bottom - rc.top);
  outRc->left = pt.x;
  outRc->top = pt.y;
  outRc->right = pt.x + w;
  outRc->bottom = pt.y + h;
  return w > 0 && h > 0;
}

static bool SetWindowClientSize(HWND hwnd, int clientW, int clientH) {
  if (!hwnd || clientW <= 0 || clientH <= 0) {
    return false;
  }
  LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
  RECT rc{0, 0, clientW, clientH};
  if (!AdjustWindowRectEx(&rc, (DWORD)style, FALSE, (DWORD)exStyle)) {
    return false;
  }
  const int outerW = rc.right - rc.left;
  const int outerH = rc.bottom - rc.top;
  return SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

static bool IsFullscreenCoopFlags(DWORD flags) {
  return (flags & DDSCL_FULLSCREEN) != 0 || (flags & DDSCL_EXCLUSIVE) != 0;
}

static RECT MakeRectFromXYWH(int x, int y, int w, int h) {
  RECT rc{};
  rc.left = x;
  rc.top = y;
  rc.right = x + w;
  rc.bottom = y + h;
  return rc;
}

static bool RectIsOriginSize(const RECT* rc, LONG w, LONG h) {
  if (!rc) {
    return true;
  }
  return rc->left == 0 && rc->top == 0 && (rc->right - rc->left) == w && (rc->bottom - rc->top) == h;
}

static void TraceRectInline(const char* label, const RECT* rc) {
  if (!label) {
    label = "rc";
  }
  if (!rc) {
    Tracef("%s=<null>", label);
    return;
  }
  Tracef("%s=[%ld,%ld,%ld,%ld] (w=%ld h=%ld)",
         label,
         (long)rc->left,
         (long)rc->top,
         (long)rc->right,
         (long)rc->bottom,
         (long)(rc->right - rc->left),
         (long)(rc->bottom - rc->top));
}
