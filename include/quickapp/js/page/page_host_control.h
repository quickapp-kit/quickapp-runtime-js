#pragma once

#include <vector>

#include "quickapp/js/abi/runtime_abi_service.h"
#include "quickapp/js/framework/js_request_id_allocator.h"
#include "quickapp/js/vm/page_vm_setup.h"

namespace quickapp::js::page {

struct PageHostControlResourceSnapshot {
  std::size_t liveNativeEntries{0};
  std::size_t liveFactoryValues{0};
};

class PageHostControlInstaller final : public vm::PageVmSetupPort {
public:
  PageHostControlInstaller(JsEngineService &engineService,
                           abi::RuntimeAbiService &runtimeAbi,
                           framework::JsRequestIdAllocatorPort &requestIds);
  ~PageHostControlInstaller();

  PageHostControlInstaller(const PageHostControlInstaller &) = delete;
  PageHostControlInstaller &
  operator=(const PageHostControlInstaller &) = delete;

  [[nodiscard]] bool startOnExecutor(JsEnginePort &engine,
                                     const JsContextRef &context) noexcept;
  [[nodiscard]] vm::PageVmSetupResult
  installOnExecutor(JsValueRef &pageVm,
                    const abi::SurfaceContext &surface) noexcept override;
  void stopOnExecutor() noexcept;
  [[nodiscard]] PageHostControlResourceSnapshot resources() const noexcept;

private:
  [[nodiscard]] NativeFunctionResult
  invokeSetTitleBar(const NativeCallView &call) noexcept;
  [[nodiscard]] NativeFunctionResult
  invokeSetMeta(const NativeCallView &call) noexcept;
  [[nodiscard]] NativeFunctionResult
  encodeAdmission(const abi::EnqueueResult &result) noexcept;
  [[nodiscard]] bool onExecutor() const noexcept;
  void unbindAllOnExecutor() noexcept;

  JsEngineService &engineService_;
  abi::RuntimeAbiService &runtimeAbi_;
  framework::JsRequestIdAllocatorPort &requestIds_;
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  std::vector<NativeBindingToken> bindings_;
  JsValueRef facadeFactory_;
  bool running_{false};
};

} // namespace quickapp::js::page
