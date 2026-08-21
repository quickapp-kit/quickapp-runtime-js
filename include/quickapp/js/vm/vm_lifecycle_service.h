#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "quickapp/js/abi/runtime_abi_service.h"
#include "quickapp/js/module/module_loader.h"
#include "quickapp/js/vm/page_initialization_stage.h"
#include "quickapp/js/vm/page_vm_setup.h"

namespace quickapp::js::vm {

struct VmResourceSnapshot {
  std::size_t appVms{0};
  std::size_t pageVms{0};
  std::size_t openSurfaces{0};
};

// Alpha uses the normal Runtime composition: this service is only the
// App/Page VM owner and initial-only binding stage.
class VmLifecycleService final {
public:
  VmLifecycleService(JsEngineService &engineService,
                     module::ModuleLoader &moduleLoader,
                     PageVmSetupPort &pageVmSetup,
                     PageInitializationStagePort &pageInitializationStage,
                     std::string packageId);
  ~VmLifecycleService();

  VmLifecycleService(const VmLifecycleService &) = delete;
  VmLifecycleService &operator=(const VmLifecycleService &) = delete;

  [[nodiscard]] bool startOnExecutor(JsEnginePort &engine,
                                      const JsContextRef &context) noexcept;
  [[nodiscard]] abi::CallbackSlots callbackSlots() noexcept;
  void stopOnExecutor() noexcept;
  void onAppContext(const abi::AppContext &context) noexcept;
  void onSurfaceContext(const abi::SurfaceContext &context) noexcept;
  void onVmInitialization(const abi::VmInitializationDispatch &dispatch) noexcept;
  void closeSurfaceOnExecutor(std::string_view surfaceId) noexcept;
  [[nodiscard]] VmResourceSnapshot resources() const noexcept;
  [[nodiscard]] EngineResult<JsValueRef> pageVmOnExecutor(
      std::string_view surfaceId) noexcept;

private:
  struct PageRecord;

  [[nodiscard]] bool onExecutor() const noexcept;
  [[nodiscard]] bool postVmCompletion(const RuntimeValue &message) noexcept;
  [[nodiscard]] bool callOptionalHook(const JsValueRef &vm,
                                      std::string_view hook,
                                      const RuntimeValue &context) noexcept;
  [[nodiscard]] RuntimeValue appContextValue(const abi::AppContext &context) const;
  [[nodiscard]] RuntimeValue surfaceContextValue(const abi::SurfaceContext &context) const;
  [[nodiscard]] RuntimeValue failureValue(std::string_view code,
                                          std::string_view message) const;
  void failInitialization(const abi::VmInitializationDispatch &dispatch,
                          std::string_view scope,
                          std::string_view phase,
                          std::string_view code,
                          std::string_view message) noexcept;
  void initializeApp(const abi::VmInitializationDispatch &dispatch) noexcept;
  void initializePage(const abi::VmInitializationDispatch &dispatch) noexcept;

  JsEngineService &engineService_;
  module::ModuleLoader &moduleLoader_;
  PageVmSetupPort &pageVmSetup_;
  PageInitializationStagePort &pageInitializationStage_;
  std::string packageId_;
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  abi::AppContext appContext_;
  bool hasAppContext_{false};
  JsValueRef appVm_;
  std::map<std::string, PageRecord, std::less<>> pages_;
  bool running_{false};
};

} // namespace quickapp::js::vm
