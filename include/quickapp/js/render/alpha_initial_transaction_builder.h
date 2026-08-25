#pragma once

#include <string_view>

#include "quickapp/js/abi/runtime_abi_types.h"
#include "quickapp/js/engine/js_engine_service.h"
#include "quickapp/js/framework/js_request_id_allocator.h"
#include "quickapp/js/vm/page_initialization_stage.h"

namespace quickapp::js::render {

class AlphaInitialTransactionBuilder final {
public:
  AlphaInitialTransactionBuilder(
      JsEngineService &engineService,
      framework::JsRequestIdAllocatorPort &requestIds);

  [[nodiscard]] bool startOnExecutor(JsEnginePort &engine,
                                     const JsContextRef &context) noexcept;
  [[nodiscard]] vm::PageInitializationStageResult
  snapshotInitialBlocksOnExecutor(const JsValueRef &pageVm,
                                  RuntimeValue::Array &blocks) noexcept;
  [[nodiscard]] vm::PageInitializationStageResult
  submitOnExecutor(std::string_view surfaceId, std::string_view templateId,
                   const abi::BindingValues &initialBindings,
                   const std::vector<abi::HandlerBinding> &initialHandlers,
                   const RuntimeValue::Array &initialBlocks) noexcept;
  void stopOnExecutor() noexcept;

private:
  [[nodiscard]] bool onExecutor() const noexcept;

  JsEngineService &engineService_;
  framework::JsRequestIdAllocatorPort &requestIds_;
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  bool running_{false};
};

} // namespace quickapp::js::render
