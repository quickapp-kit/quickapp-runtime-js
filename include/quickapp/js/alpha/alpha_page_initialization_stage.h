#pragma once

#include "quickapp/js/abi/runtime_abi_types.h"
#include <map>
#include <string>

#include "quickapp/js/binding/alpha_initial_binding_stage.h"
#include "quickapp/js/render/alpha_initial_transaction_builder.h"
#include "quickapp/js/vm/page_initialization_stage.h"

namespace quickapp::js::alpha {

class AlphaPageInitializationStage final
    : public vm::PageInitializationStagePort {
public:
  AlphaPageInitializationStage(
      binding::AlphaInitialBindingStage &bindingStage,
      render::AlphaInitialTransactionBuilder &transactionBuilder);

  [[nodiscard]] vm::PageInitializationStageResult
  evaluateInitialOnExecutor(
      std::string_view surfaceId, std::string_view templateId,
      const module::ModuleDefinitionHandle &definition,
      const JsValueRef &pageVm) noexcept override;
  [[nodiscard]] vm::PageInitializationStageResult
  submitInitialOnExecutor(std::string_view surfaceId) noexcept override;
  void cancelOnExecutor(std::string_view surfaceId) noexcept override;
  void cancelAllOnExecutor() noexcept override;

private:
  struct PendingInitial {
    std::string templateId;
    binding::InitialBindingSnapshot snapshot;
    std::vector<abi::HandlerBinding> handlers;
    RuntimeValue::Array initialBlocks;
  };

  binding::AlphaInitialBindingStage &bindingStage_;
  render::AlphaInitialTransactionBuilder &transactionBuilder_;
  std::map<std::string, PendingInitial, std::less<>> pending_;
};

} // namespace quickapp::js::alpha
