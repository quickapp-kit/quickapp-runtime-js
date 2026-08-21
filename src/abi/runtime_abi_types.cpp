#include "quickapp/js/abi/runtime_abi_types.h"

#include <utility>

namespace quickapp::js::abi {

EnqueueResult EnqueueResult::accepted() { return {true, std::nullopt}; }

EnqueueResult EnqueueResult::rejected(AbiRuntimeError error) {
  return {false, std::move(error)};
}

std::string_view abiErrorCodeName(AbiErrorCode code) noexcept {
  switch (code) {
  case AbiErrorCode::InvalidArgument:
    return "ABI_INVALID_ARGUMENT";
  case AbiErrorCode::UnsupportedVersion:
    return "ABI_UNSUPPORTED_VERSION";
  case AbiErrorCode::SurfaceNotFound:
    return "SURFACE_NOT_FOUND";
  case AbiErrorCode::OutOfMemory:
    return "OUT_OF_MEMORY";
  case AbiErrorCode::QueueOverflow:
    return "QUEUE_OVERFLOW";
  case AbiErrorCode::JsException:
    return "JS_EXCEPTION";
  case AbiErrorCode::PortClosed:
    return "PLATFORM_REJECTED";
  }
  return "JS_EXCEPTION";
}

bool CapabilitySupportSnapshot::supports(std::string_view module,
                                         std::string_view method) const {
  return methods.contains({std::string(module), std::string(method)});
}

ConsumerRegistrationToken::ConsumerRegistrationToken(
    ConsumerRegistrationToken &&other) noexcept
    : id_(std::exchange(other.id_, 0)) {}

ConsumerRegistrationToken &ConsumerRegistrationToken::operator=(
    ConsumerRegistrationToken &&other) noexcept {
  if (this != &other) {
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

CoreMessageKind messageKind(const CoreInboundMessage &message) {
  return std::visit([](const auto &typed) { return typed.kind; }, message);
}

JsCallbackKind callbackKind(const JsInboundMessage &message) {
  return std::visit([](const auto &typed) { return typed.kind; }, message);
}

std::string_view coreMessageKindName(CoreMessageKind kind) {
  switch (kind) {
  case CoreMessageKind::InstantiateTemplate:
    return "instantiateTemplate";
  case CoreMessageKind::CompleteVerifiedModuleLoad:
    return "loadVerifiedModuleResult";
  case CoreMessageKind::CompleteVmInitialization:
    return "vmInitializationResult";
  case CoreMessageKind::SubmitRenderTransaction:
    return "renderTransaction";
  case CoreMessageKind::RegisterHandler:
    return "registerHandler";
  case CoreMessageKind::UnregisterHandler:
    return "unregisterHandler";
  case CoreMessageKind::NavigationPush:
    return "navigationPush";
  case CoreMessageKind::NavigationClose:
    return "navigationClose";
  case CoreMessageKind::ShowToast:
    return "showToast";
  case CoreMessageKind::DeviceGetInfo:
    return "deviceGetInfo";
  case CoreMessageKind::SetTitleBar:
    return "setTitleBar";
  case CoreMessageKind::SetMeta:
    return "setMeta";
  case CoreMessageKind::CompleteLifecycle:
    return "lifecycleResult";
  }
  return {};
}

std::string_view callbackKindName(JsCallbackKind kind) {
  switch (kind) {
  case JsCallbackKind::LoadVerifiedModule:
    return "loadVerifiedModule";
  case JsCallbackKind::AppContext:
    return "appContext";
  case JsCallbackKind::SurfaceContext:
    return "surfaceContext";
  case JsCallbackKind::VmInitializationDispatch:
    return "vmInitializationDispatch";
  case JsCallbackKind::LifecycleDispatch:
    return "lifecycleDispatch";
  case JsCallbackKind::JsEventDispatch:
    return "jsEventDispatch";
  case JsCallbackKind::InstantiateTemplateResult:
    return "instantiateTemplateResult";
  case JsCallbackKind::HandlerRegistrationResult:
    return "handlerRegistrationResult";
  case JsCallbackKind::RenderTransactionResult:
    return "renderTransactionResult";
  case JsCallbackKind::NavigationPushResult:
    return "navigationPushResult";
  case JsCallbackKind::NavigationCloseResult:
    return "navigationCloseResult";
  case JsCallbackKind::ShowToastResult:
    return "showToastResult";
  case JsCallbackKind::DeviceGetInfoResult:
    return "deviceGetInfoResult";
  case JsCallbackKind::SetTitleBarResult:
    return "setTitleBarResult";
  case JsCallbackKind::SetMetaResult:
    return "setMetaResult";
  case JsCallbackKind::SurfaceStatusChanged:
    return "surfaceStatusChanged";
  }
  return {};
}

} // namespace quickapp::js::abi
