#include "quickapp/js/alpha/alpha_page_initialization_stage.h"

#include <utility>

namespace quickapp::js::alpha {

AlphaPageInitializationStage::AlphaPageInitializationStage(
    binding::AlphaInitialBindingStage &bindingStage,
    render::AlphaInitialTransactionBuilder &transactionBuilder)
    : bindingStage_(bindingStage), transactionBuilder_(transactionBuilder) {}

vm::PageInitializationStageResult
AlphaPageInitializationStage::evaluateInitialOnExecutor(
    std::string_view surfaceId, std::string_view templateId,
    const module::ModuleDefinitionHandle &definition,
    const JsValueRef &pageVm) noexcept {
  if (surfaceId.empty() || templateId.empty() ||
      pending_.contains(std::string(surfaceId))) {
    return vm::PageInitializationStageResult::failure(
        {"ABI_INVALID_ARGUMENT", "Initial page stage is already pending"});
  }
  auto snapshot = bindingStage_.evaluateOnExecutor(definition, pageVm);
  if (!snapshot.ok())
    return vm::PageInitializationStageResult::failure(snapshot.error());
  auto handlers = bindingStage_.moduleLoader().handlerBindingsOnExecutor(
      definition, "cmp:" + std::string(surfaceId));
  if (!handlers.ok()) {
    return vm::PageInitializationStageResult::failure(
      {std::string(module::moduleErrorCodeName(handlers.error().code)),
         handlers.error().message});
  }
  try {
    RuntimeValue::Array initialBlocks;
    auto blocks = transactionBuilder_.snapshotInitialBlocksOnExecutor(pageVm,
                                                                       initialBlocks);
    if (!blocks.ok()) return blocks;
    pending_.emplace(std::string(surfaceId),
                     PendingInitial{std::string(templateId),
                                    std::move(snapshot).value(),
                                    std::move(handlers).value(),
                                    std::move(initialBlocks)});
    return vm::PageInitializationStageResult::success();
  } catch (...) {
    return vm::PageInitializationStageResult::failure(
        {"OUT_OF_MEMORY", "Initial page snapshot allocation failed"});
  }
}

vm::PageInitializationStageResult
AlphaPageInitializationStage::submitInitialOnExecutor(
    std::string_view surfaceId) noexcept {
  const auto found = pending_.find(std::string(surfaceId));
  if (found == pending_.end()) {
    return vm::PageInitializationStageResult::failure(
        {"ABI_INVALID_ARGUMENT", "Initial page snapshot is unavailable"});
  }
  auto result = transactionBuilder_.submitOnExecutor(
      found->first, found->second.templateId, found->second.snapshot.values,
      found->second.handlers, found->second.initialBlocks);
  if (result.ok()) pending_.erase(found);
  return result;
}

void AlphaPageInitializationStage::cancelOnExecutor(
    std::string_view surfaceId) noexcept {
  pending_.erase(std::string(surfaceId));
}

void AlphaPageInitializationStage::cancelAllOnExecutor() noexcept {
  pending_.clear();
}

} // namespace quickapp::js::alpha
