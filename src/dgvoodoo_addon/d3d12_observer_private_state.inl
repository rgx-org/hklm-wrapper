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
