#include "quickapp/js/render/alpha_initial_transaction_builder.h"

#include <array>
#include <utility>

namespace quickapp::js::render {
namespace {

RuntimeValue::Object object(
    std::initializer_list<std::pair<const std::string, RuntimeValue>> values) {
  return RuntimeValue::Object(values);
}

vm::PageInitializationStageError stageError(std::string code,
                                             std::string message) {
  return {std::move(code), std::move(message)};
}

} // namespace

AlphaInitialTransactionBuilder::AlphaInitialTransactionBuilder(
    JsEngineService &engineService,
    framework::JsRequestIdAllocatorPort &requestIds)
    : engineService_(engineService), requestIds_(requestIds) {}

bool AlphaInitialTransactionBuilder::onExecutor() const noexcept {
  return engineService_.executor().isOnExecutor();
}

bool AlphaInitialTransactionBuilder::startOnExecutor(
    JsEnginePort &engine, const JsContextRef &context) noexcept {
  if (!onExecutor() || running_ || !context.valid()) return false;
  engine_ = &engine;
  context_ = &context;
  running_ = true;
  return true;
}

vm::PageInitializationStageResult
AlphaInitialTransactionBuilder::submitOnExecutor(
    std::string_view surfaceId, std::string_view templateId,
    const abi::BindingValues &initialBindings,
    const std::vector<abi::HandlerBinding> &initialHandlers) noexcept {
  if (!onExecutor() || !running_ || !engine_ || !context_ ||
      surfaceId.empty() || templateId.empty()) {
    return vm::PageInitializationStageResult::failure(
        stageError("ABI_INVALID_ARGUMENT",
                   "Initial transaction builder is unavailable"));
  }
  try {
    RuntimeValue::Object values;
    for (const auto &[id, value] : initialBindings) {
      if (const auto *text = std::get_if<std::string>(&value))
        values.emplace(std::to_string(id), RuntimeValue(*text));
      else
        values.emplace(std::to_string(id),
                       RuntimeValue(std::get<bool>(value)));
    }
    RuntimeValue::Array handlers;
    handlers.reserve(initialHandlers.size());
    for (const auto &handler : initialHandlers) {
      handlers.emplace_back(RuntimeValue::Object{
          {"ownerInstanceId", RuntimeValue(handler.ownerInstanceId)},
          {"templateHandlerId",
           RuntimeValue(static_cast<double>(handler.templateHandlerId))},
          {"handlerId", RuntimeValue(handler.handlerId)}});
    }
    RuntimeValue message(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("instantiateTemplate")},
        {"requestId", RuntimeValue(requestIds_.nextRequestId())},
        {"surfaceId", RuntimeValue(std::string(surfaceId))},
        {"templateId", RuntimeValue(std::string(templateId))},
        {"ownerInstanceId", RuntimeValue("cmp:" + std::string(surfaceId))},
        {"initialBindings", RuntimeValue(std::move(values))},
        {"initialBlocks", RuntimeValue(RuntimeValue::Array{})},
        {"initialHandlers", RuntimeValue(std::move(handlers))},
    }));
    auto global = engine_->globalObject(*context_);
    if (!global.ok())
      return vm::PageInitializationStageResult::failure(
          stageError("JS_EXCEPTION", global.error().message));
    auto native = engine_->getProperty(
        *context_, global.value(), "$quickapp_runtime_v1_instantiateTemplate$");
    auto argument = engine_->fromRuntimeValue(*context_, message);
    auto thisValue =
        engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
    if (!native.ok() || !argument.ok() || !thisValue.ok()) {
      return vm::PageInitializationStageResult::failure(
          stageError("JS_EXCEPTION", "InstantiateTemplate binding unavailable"));
    }
    std::array<JsValueRef, 1> args{std::move(argument).value()};
    auto result = engine_->call(
        *context_, native.value(), std::move(thisValue).value(),
        std::span<const JsValueRef>(args.data(), args.size()));
    if (!result.ok())
      return vm::PageInitializationStageResult::failure(
          stageError("JS_EXCEPTION", result.error().message));
    auto converted = engine_->toRuntimeValue(*context_, result.value(), {8, 64});
    const auto *response =
        converted.ok()
            ? std::get_if<RuntimeValue::Object>(&converted.value().storage())
            : nullptr;
    if (!response) {
      return vm::PageInitializationStageResult::failure(
          stageError("QUEUE_OVERFLOW", "InstantiateTemplate was rejected"));
    }
    const auto accepted = response->find("ok");
    if (accepted == response->end() ||
        !std::get_if<bool>(&accepted->second.storage()) ||
        !std::get<bool>(accepted->second.storage())) {
      return vm::PageInitializationStageResult::failure(
          stageError("QUEUE_OVERFLOW", "InstantiateTemplate was rejected"));
    }
    return vm::PageInitializationStageResult::success();
  } catch (...) {
    return vm::PageInitializationStageResult::failure(
        stageError("OUT_OF_MEMORY", "Initial transaction allocation failed"));
  }
}

void AlphaInitialTransactionBuilder::stopOnExecutor() noexcept {
  if (!onExecutor()) return;
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

} // namespace quickapp::js::render
