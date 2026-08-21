#pragma once

#include "quickapp/js/abi/runtime_abi_types.h"
#include "quickapp/js/module/module_loader.h"
#include "quickapp/js/vm/page_initialization_stage.h"

namespace quickapp::js::binding {

struct InitialBindingSnapshot {
  abi::BindingValues values;
};

class AlphaInitialBindingStage final {
public:
  AlphaInitialBindingStage(JsEngineService &engineService,
                           module::ModuleLoader &moduleLoader);

  [[nodiscard]] bool startOnExecutor(JsEnginePort &engine,
                                     const JsContextRef &context) noexcept;
  [[nodiscard]] Result<InitialBindingSnapshot,
                       vm::PageInitializationStageError>
  evaluateOnExecutor(const module::ModuleDefinitionHandle &definition,
                     const JsValueRef &pageVm) noexcept;
  void stopOnExecutor() noexcept;
  [[nodiscard]] module::ModuleLoader& moduleLoader() noexcept {
    return moduleLoader_;
  }

private:
  [[nodiscard]] bool onExecutor() const noexcept;

  JsEngineService &engineService_;
  module::ModuleLoader &moduleLoader_;
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  bool running_{false};
};

} // namespace quickapp::js::binding
