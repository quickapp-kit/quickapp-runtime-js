#include "quickapp/js/event/handler_registry.h"

#include <array>
#include <utility>

namespace quickapp::js::event {

bool HandlerRegistry::startOnExecutor(JsEnginePort& engine,
                                      const JsContextRef& context) noexcept {
  if (!engineService_.executor().isOnExecutor() || running_ || !context.valid()) {
    return false;
  }
  engine_ = &engine;
  context_ = &context;
  running_ = true;
  return true;
}

bool HandlerRegistry::bind(std::string surfaceId, std::string handlerId,
                           std::string methodName, JsValueRef pageVm) noexcept {
  if (!running_ || !engineService_.executor().isOnExecutor() ||
      surfaceId.empty() || handlerId.empty() || methodName.empty() ||
      !pageVm.valid()) {
    return false;
  }
  try {
    const std::string key = surfaceId + "\n" + handlerId;
    handlers_[key] =
        Handler{std::move(surfaceId), std::move(methodName), std::move(pageVm)};
    return true;
  } catch (...) {
    return false;
  }
}

void HandlerRegistry::unbind(std::string_view surfaceId,
                             std::string_view handlerId) noexcept {
  if (!running_ || !engineService_.executor().isOnExecutor() ||
      surfaceId.empty() || handlerId.empty()) {
    return;
  }
  handlers_.erase(std::string(surfaceId) + "\n" + std::string(handlerId));
}

bool HandlerRegistry::dispatchOnExecutor(
    const abi::JsEventDispatch& dispatch) noexcept {
  if (!running_ || !engineService_.executor().isOnExecutor() || !engine_ ||
      !context_) {
    return false;
  }
  const auto found = handlers_.find(dispatch.surfaceId + "\n" + dispatch.handlerId);
  if (found == handlers_.end()) {
    return false;
  }
  auto method = engine_->getProperty(*context_, found->second.pageVm,
                                     found->second.methodName);
  if (!method.ok()) return false;
  auto callable = engine_->isCallable(*context_, method.value());
  if (!callable.ok() || !callable.value()) return false;
  RuntimeValue::Object value;
  for (const auto& [name, item] : dispatch.payload) value.emplace(name, item);
  value.emplace("type", RuntimeValue(dispatch.eventType));
  value.emplace("phase", RuntimeValue(dispatch.phase));
  value.emplace("requestId", RuntimeValue(dispatch.requestId));
  value.emplace("surfaceId", RuntimeValue(dispatch.surfaceId));
  value.emplace("handlerId", RuntimeValue(dispatch.handlerId));
  value.emplace("timestamp", RuntimeValue(dispatch.timestamp));
  auto argument = engine_->fromRuntimeValue(*context_, RuntimeValue(std::move(value)));
  if (!argument.ok()) return false;
  auto global = engine_->globalObject(*context_);
  auto surface = engine_->fromRuntimeValue(*context_, RuntimeValue(dispatch.surfaceId));
  auto request = engine_->fromRuntimeValue(*context_, RuntimeValue(dispatch.requestId));
  if (!global.ok() || !surface.ok() || !request.ok() ||
      !engine_->setProperty(*context_, global.value(),
                            "$quickapp_current_surface_id$",
                            surface.value()).ok() ||
      !engine_->setProperty(*context_, global.value(),
                            "$quickapp_current_request_id$",
                            request.value()).ok()) {
    return false;
  }
  std::array<JsValueRef, 1> args{std::move(argument).value()};
  auto result = engine_->call(*context_, method.value(), found->second.pageVm,
                             std::span<const JsValueRef>(args.data(), args.size()));
  auto nullValue = engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
  if (nullValue.ok()) {
    auto globalAgain = engine_->globalObject(*context_);
    if (globalAgain.ok()) {
      static_cast<void>(engine_->setProperty(
          *context_, globalAgain.value(), "$quickapp_current_surface_id$",
          nullValue.value()));
      static_cast<void>(engine_->setProperty(
          *context_, globalAgain.value(), "$quickapp_current_request_id$",
          nullValue.value()));
    }
  }
  if (!result.ok()) return false;
  static_cast<void>(engine_->drainMicrotasks(*context_, 32));
  return true;
}

void HandlerRegistry::closeSurface(std::string_view surfaceId) noexcept {
  for (auto it = handlers_.begin(); it != handlers_.end();) {
    if (it->second.surfaceId == surfaceId) it = handlers_.erase(it);
    else ++it;
  }
}

void HandlerRegistry::stopOnExecutor() noexcept {
  if (!engineService_.executor().isOnExecutor()) return;
  handlers_.clear();
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

}  // namespace quickapp::js::event
