#include "quickapp/js/binding/alpha_initial_binding_stage.h"

#include <array>
#include <utility>

namespace quickapp::js::binding {
namespace {

vm::PageInitializationStageError stageError(std::string code,
                                             std::string message) {
  return {std::move(code), std::move(message)};
}

} // namespace

AlphaInitialBindingStage::AlphaInitialBindingStage(
    JsEngineService &engineService, module::ModuleLoader &moduleLoader)
    : engineService_(engineService), moduleLoader_(moduleLoader) {}

bool AlphaInitialBindingStage::onExecutor() const noexcept {
  return engineService_.executor().isOnExecutor();
}

bool AlphaInitialBindingStage::startOnExecutor(
    JsEnginePort &engine, const JsContextRef &context) noexcept {
  if (!onExecutor() || running_ || !context.valid()) return false;
  engine_ = &engine;
  context_ = &context;
  running_ = true;
  return true;
}

Result<InitialBindingSnapshot, vm::PageInitializationStageError>
AlphaInitialBindingStage::evaluateOnExecutor(
    const module::ModuleDefinitionHandle &definition,
    const JsValueRef &pageVm) noexcept {
  if (!onExecutor() || !running_ || !engine_ || !context_ ||
      !pageVm.valid()) {
    return Result<InitialBindingSnapshot,
                  vm::PageInitializationStageError>::failure(
        stageError("ABI_INVALID_ARGUMENT", "Binding stage is unavailable"));
  }

  auto evaluators = moduleLoader_.bindingEvaluatorsOnExecutor(definition);
  if (!evaluators.ok()) {
    return Result<InitialBindingSnapshot,
                  vm::PageInitializationStageError>::failure(
        stageError("MODULE_ABI_UNSUPPORTED", evaluators.error().message));
  }

  try {
    InitialBindingSnapshot snapshot;
    for (auto &entry : evaluators.value()) {
      if (!entry.initial) continue;
      auto scope = engine_->fromRuntimeValue(
          *context_, RuntimeValue(RuntimeValue::Object{}));
      if (!scope.ok()) {
        return Result<InitialBindingSnapshot,
                      vm::PageInitializationStageError>::failure(
            stageError("OUT_OF_MEMORY", scope.error().message));
      }
      std::array<JsValueRef, 1> args{std::move(scope).value()};
      auto value = engine_->call(
          *context_, entry.evaluator, pageVm,
          std::span<const JsValueRef>(args.data(), args.size()));
      if (!value.ok()) {
        return Result<InitialBindingSnapshot,
                      vm::PageInitializationStageError>::failure(
            stageError("JS_EXCEPTION", value.error().message));
      }
      auto converted = engine_->toRuntimeValue(*context_, value.value(), {8, 64});
      if (!converted.ok()) {
        return Result<InitialBindingSnapshot,
                      vm::PageInitializationStageError>::failure(
            stageError("JS_EXCEPTION", converted.error().message));
      }
      if (const auto *text =
              std::get_if<std::string>(&converted.value().storage())) {
        snapshot.values.emplace(entry.templateBindingId, *text);
      } else if (const auto *flag =
                     std::get_if<bool>(&converted.value().storage())) {
        snapshot.values.emplace(entry.templateBindingId, *flag);
      } else if (const auto *number =
                     std::get_if<double>(&converted.value().storage())) {
        snapshot.values.emplace(entry.templateBindingId, *number);
      } else {
        return Result<InitialBindingSnapshot,
                      vm::PageInitializationStageError>::failure(
            stageError("MODULE_ABI_UNSUPPORTED",
                       "Initial binding result must be string, boolean, or number"));
      }
    }
    return Result<InitialBindingSnapshot,
                  vm::PageInitializationStageError>::success(
        std::move(snapshot));
  } catch (...) {
    return Result<InitialBindingSnapshot,
                  vm::PageInitializationStageError>::failure(
        stageError("OUT_OF_MEMORY", "Initial binding allocation failed"));
  }
}

void AlphaInitialBindingStage::stopOnExecutor() noexcept {
  if (!onExecutor()) return;
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

} // namespace quickapp::js::binding
