struct AdapterState;
static ID3D12PipelineState* PipelineForMethod(AdapterState& ad, twinshim::SurfaceScaleMethod method);

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
  ID3D12PipelineState* psoCatmullRom = nullptr;
  ID3D12PipelineState* psoBicubic = nullptr;
  ID3D12PipelineState* psoLanczos = nullptr;
  ID3D12PipelineState* psoLanczos3 = nullptr;
  ID3D12PipelineState* psoPixFast = nullptr;

  // Keep shader blobs alive while referenced by dgVoodoo's pipeline cache.
  ID3DBlob* vs = nullptr;
  ID3DBlob* psPoint = nullptr;
  ID3DBlob* psLinear = nullptr;
  ID3DBlob* psCatmullRom = nullptr;
  ID3DBlob* psBicubic = nullptr;
  ID3DBlob* psLanczos = nullptr;
  ID3DBlob* psLanczos3 = nullptr;
  ID3DBlob* psPixFast = nullptr;

  ID3D12Root::GraphicsPLDesc plDescPoint{};
  ID3D12Root::GraphicsPLDesc plDescLinear{};
  ID3D12Root::GraphicsPLDesc plDescCatmullRom{};
  ID3D12Root::GraphicsPLDesc plDescBicubic{};
  ID3D12Root::GraphicsPLDesc plDescLanczos{};
  ID3D12Root::GraphicsPLDesc plDescLanczos3{};
  ID3D12Root::GraphicsPLDesc plDescPixFast{};

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

static ID3D12PipelineState* PipelineForMethod(AdapterState& ad, twinshim::SurfaceScaleMethod method) {
  switch (method) {
    case twinshim::SurfaceScaleMethod::kBilinear:
      return ad.psoLinear;
    case twinshim::SurfaceScaleMethod::kBicubic:
      return ad.psoBicubic;
    case twinshim::SurfaceScaleMethod::kCatmullRom:
      return ad.psoCatmullRom;
    case twinshim::SurfaceScaleMethod::kLanczos:
      return ad.psoLanczos;
    case twinshim::SurfaceScaleMethod::kLanczos3:
      return ad.psoLanczos3;
    case twinshim::SurfaceScaleMethod::kPixelFast:
      return ad.psoPixFast;
    default:
      return ad.psoPoint;
  }
}

struct Vertex {
  float pX;
  float pY;
  float tU;
  float tV;
};

static constexpr UInt32 kVbVertexCap = 2048;

