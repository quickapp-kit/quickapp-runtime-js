#include "quickapp/js/vm/vm_lifecycle_service.h"

#include <array>
#include <exception>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <utility>

namespace quickapp::js::vm {
namespace {

void androidVmFailure(std::string_view phase, std::string_view message) noexcept {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "QuickAppKit", "android.vm.failure phase=%.*s error=%.*s",
                      static_cast<int>(phase.size()), phase.data(),
                      static_cast<int>(message.size()), message.data());
#else
  (void)phase;
  (void)message;
#endif
}

RuntimeValue::Object object(std::initializer_list<std::pair<const std::string, RuntimeValue>> values) {
  return RuntimeValue::Object(values);
}

std::string errorCode(std::string_view code) {
  return std::string(code);
}

} // namespace

struct VmLifecycleService::PageRecord {
  abi::SurfaceContext context;
  JsValueRef vm;
  module::ModuleDefinitionHandle definition;
};

VmLifecycleService::VmLifecycleService(JsEngineService &engineService,
                                       module::ModuleLoader &moduleLoader,
                                       PageVmSetupPort &pageVmSetup,
                                       PageInitializationStagePort &pageInitializationStage,
                                       std::string packageId)
    : engineService_(engineService), moduleLoader_(moduleLoader),
      pageVmSetup_(pageVmSetup),
      pageInitializationStage_(pageInitializationStage),
      packageId_(std::move(packageId)) {}

VmLifecycleService::~VmLifecycleService() {
  if (running_) std::terminate();
}

bool VmLifecycleService::onExecutor() const noexcept {
  return engineService_.executor().isOnExecutor();
}

bool VmLifecycleService::startOnExecutor(JsEnginePort &engine,
                                          const JsContextRef &context) noexcept {
  if (!onExecutor() || running_ || !context.valid()) return false;
  engine_ = &engine;
  context_ = &context;
  running_ = true;
  return true;
}

abi::CallbackSlots VmLifecycleService::callbackSlots() noexcept {
  abi::CallbackSlots slots;
  slots.appContext = [this](const abi::AppContext &context) {
    onAppContext(context);
  };
  slots.surfaceContext = [this](const abi::SurfaceContext &context) {
    onSurfaceContext(context);
  };
  slots.vmInitializationDispatch = [this](const abi::VmInitializationDispatch &dispatch) {
    onVmInitialization(dispatch);
  };
  return slots;
}

RuntimeValue VmLifecycleService::appContextValue(const abi::AppContext &context) const {
  RuntimeValue::Array capabilities;
  for (const auto &capability : context.declaredCapabilities)
    capabilities.emplace_back(capability);
  return RuntimeValue(object({
      {"packageId", RuntimeValue(context.packageId)},
      {"versionName", RuntimeValue(context.versionName)},
      {"versionCode", RuntimeValue(static_cast<double>(context.versionCode))},
      {"runtimeVersion", RuntimeValue(context.runtimeVersion)},
      {"declaredCapabilities", RuntimeValue(std::move(capabilities))}}));
}

RuntimeValue VmLifecycleService::surfaceContextValue(const abi::SurfaceContext &context) const {
  RuntimeValue::Array capabilities;
  for (const auto &capability : context.hostCapabilities)
    capabilities.emplace_back(capability);
  RuntimeValue::Object viewport{
      {"width", RuntimeValue(context.viewport.width)},
      {"height", RuntimeValue(context.viewport.height)},
      {"unit", RuntimeValue(context.viewport.unit)}};
  return RuntimeValue(object({
      {"surfaceId", RuntimeValue(context.surfaceId)},
      {"packageId", RuntimeValue(context.packageId)},
      {"route", RuntimeValue(context.route)},
      {"templateId", RuntimeValue(context.templateId)},
      {"params", RuntimeValue(context.params)},
      {"hostCapabilities", RuntimeValue(std::move(capabilities))},
      {"viewport", RuntimeValue(std::move(viewport))}}));
}

EngineResult<JsValueRef> VmLifecycleService::pageVmOnExecutor(
    std::string_view surfaceId) noexcept {
  if (!onExecutor() || !running_ || engine_ == nullptr || context_ == nullptr) {
    return EngineResult<JsValueRef>::failure(
        {EngineExceptionKind::Runtime, "page VM is unavailable", std::nullopt,
         std::nullopt, std::nullopt, std::nullopt});
  }
  const auto found = pages_.find(std::string(surfaceId));
  if (found == pages_.end() || !found->second.vm.valid()) {
    return EngineResult<JsValueRef>::failure(
        {EngineExceptionKind::Runtime, "page VM is unavailable", std::nullopt,
         std::nullopt, std::nullopt, std::nullopt});
  }
  return engine_->retain(*context_, found->second.vm);
}

RuntimeValue VmLifecycleService::failureValue(std::string_view code,
                                              std::string_view message) const {
  return RuntimeValue(object({{"code", RuntimeValue(errorCode(code))},
                              {"message", RuntimeValue(std::string(message))},
                              {"retryable", RuntimeValue(false)}}));
}

bool VmLifecycleService::postVmCompletion(const RuntimeValue &message) noexcept {
  if (!onExecutor() || !running_ || !engine_ || !context_) return false;
  try {
    auto global = engine_->globalObject(*context_);
    if (!global.ok()) return false;
    auto native = engine_->getProperty(
        *context_, global.value(),
        "$quickapp_runtime_v1_completeVmInitialization$");
    auto argument = engine_->fromRuntimeValue(*context_, message);
    auto thisValue = engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
    if (!native.ok() || !argument.ok() || !thisValue.ok()) return false;
    std::array<JsValueRef, 1> args{std::move(argument).value()};
    auto result = engine_->call(*context_, native.value(),
                                std::move(thisValue).value(),
                                std::span<const JsValueRef>(args.data(), args.size()));
    if (!result.ok()) return false;
    auto converted = engine_->toRuntimeValue(*context_, result.value(), {8, 64});
    if (!converted.ok()) return false;
    const auto *object = std::get_if<RuntimeValue::Object>(&converted.value().storage());
    if (!object) return false;
    const auto found = object->find("ok");
    return found != object->end() && std::get_if<bool>(&found->second.storage()) &&
           std::get<bool>(found->second.storage());
  } catch (...) {
    return false;
  }
}

bool VmLifecycleService::callOptionalHook(const JsValueRef &vm,
                                          std::string_view hook,
                                          const RuntimeValue &context) noexcept {
  auto method = engine_->getProperty(*context_, vm, hook);
  if (!method.ok()) return false;
  auto callable = engine_->isCallable(*context_, method.value());
  if (!callable.ok()) return false;
  if (!callable.value()) return true;
  auto argument = engine_->fromRuntimeValue(*context_, context);
  if (!argument.ok()) return false;
  std::array<JsValueRef, 1> args{std::move(argument).value()};
  auto result = engine_->call(*context_, method.value(), vm,
                              std::span<const JsValueRef>(args.data(), args.size()));
  return result.ok();
}

void VmLifecycleService::onAppContext(const abi::AppContext &context) noexcept {
  if (!onExecutor() || !running_ || hasAppContext_ || context.packageId != packageId_)
    return;
  appContext_ = context;
  hasAppContext_ = true;
}

void VmLifecycleService::onSurfaceContext(const abi::SurfaceContext &context) noexcept {
  if (!onExecutor() || !running_ || context.packageId != packageId_ ||
      pages_.contains(context.surfaceId)) return;
  pages_.emplace(context.surfaceId, PageRecord{context, {}, {}});
}

void VmLifecycleService::failInitialization(
    const abi::VmInitializationDispatch &dispatch, std::string_view scope,
    std::string_view phase, std::string_view code,
    std::string_view message) noexcept {
  androidVmFailure(phase, message);
  RuntimeValue::Object value{
      {"schemaVersion", RuntimeValue(1.0)},
      {"kind", RuntimeValue("vmInitializationResult")},
      {"requestId", RuntimeValue(dispatch.requestId)},
      {"scope", RuntimeValue(std::string(scope))},
      {"status", RuntimeValue("failed")},
      {"failedPhase", RuntimeValue(std::string(phase))},
      {"error", failureValue(code, message)}};
  if (dispatch.surfaceId) value.emplace("surfaceId", RuntimeValue(*dispatch.surfaceId));
  static_cast<void>(postVmCompletion(RuntimeValue(std::move(value))));
}

void VmLifecycleService::initializeApp(
    const abi::VmInitializationDispatch &dispatch) noexcept {
  if (!hasAppContext_ || appVm_.valid()) {
    failInitialization(dispatch, "app", "onCreate", "ABI_INVALID_ARGUMENT",
                       "App context or VM is unavailable");
    return;
  }
  const auto definition = moduleLoader_.appDefinitionOnExecutor();
  if (!definition) {
    failInitialization(dispatch, "app", "onCreate", "MODULE_ABI_UNSUPPORTED",
                       "App Definition is unavailable");
    return;
  }
  auto vm = moduleLoader_.createVmOnExecutor(*definition, appContextValue(appContext_));
  if (!vm.ok()) {
    failInitialization(dispatch, "app", "onCreate", "JS_EXCEPTION", vm.error().message);
    return;
  }
  appVm_ = std::move(vm).value();
  if (!callOptionalHook(appVm_, "onCreate", appContextValue(appContext_))) {
    appVm_.reset();
    failInitialization(dispatch, "app", "onCreate", "JS_EXCEPTION", "onCreate failed");
    return;
  }
  static_cast<void>(engine_->drainMicrotasks(*context_, 16));
  RuntimeValue::Object value{
      {"schemaVersion", RuntimeValue(1.0)},
      {"kind", RuntimeValue("vmInitializationResult")},
      {"requestId", RuntimeValue(dispatch.requestId)},
      {"scope", RuntimeValue("app")},
      {"status", RuntimeValue("completed")}};
  static_cast<void>(postVmCompletion(RuntimeValue(std::move(value))));
}

void VmLifecycleService::initializePage(
    const abi::VmInitializationDispatch &dispatch) noexcept {
  if (!dispatch.surfaceId) return;
  auto found = pages_.find(*dispatch.surfaceId);
  if (found == pages_.end() || found->second.vm.valid()) {
    failInitialization(dispatch, "page", "onInit", "SURFACE_NOT_FOUND",
                       "Page context or VM is unavailable");
    return;
  }
  auto definition = moduleLoader_.pageDefinitionForSurfaceOnExecutor(
      *dispatch.surfaceId, found->second.context.templateId);
  if (!definition) {
    failInitialization(dispatch, "page", "onInit", "MODULE_ABI_UNSUPPORTED",
                       "Page Definition is unavailable");
    return;
  }
  auto vm = moduleLoader_.createVmOnExecutor(*definition,
                                              surfaceContextValue(found->second.context));
  if (!vm.ok()) {
    failInitialization(dispatch, "page", "onInit", "JS_EXCEPTION", vm.error().message);
    return;
  }
  found->second.definition = *definition;
  found->second.vm = std::move(vm).value();
  auto setup =
      pageVmSetup_.installOnExecutor(found->second.vm, found->second.context);
  if (!setup.ok()) {
    found->second.vm.reset();
    failInitialization(dispatch, "page", "onInit", setup.error().code,
                       setup.error().message);
    return;
  }
  const auto setSurfaceGlobal = [&](const RuntimeValue &value) {
    auto global = engine_->globalObject(*context_);
    auto jsValue = engine_->fromRuntimeValue(*context_, value);
    return global.ok() && jsValue.ok() &&
           engine_->setProperty(*context_, global.value(),
                                "$quickapp_current_surface_id$",
                                jsValue.value()).ok();
  };
  const auto clearSurfaceGlobal = [&]() {
    static_cast<void>(setSurfaceGlobal(RuntimeValue(nullptr)));
  };
  if (!setSurfaceGlobal(RuntimeValue(*dispatch.surfaceId))) {
    found->second.vm.reset();
    failInitialization(dispatch, "page", "onInit", "JS_EXCEPTION",
                       "Surface context could not be installed");
    return;
  }
  const auto contextValue = surfaceContextValue(found->second.context);
  if (!callOptionalHook(found->second.vm, "onInit", contextValue)) {
    clearSurfaceGlobal();
    found->second.vm.reset();
    failInitialization(dispatch, "page", "onInit", "JS_EXCEPTION", "onInit failed");
    return;
  }
  auto staged = pageInitializationStage_.evaluateInitialOnExecutor(
      *dispatch.surfaceId, found->second.context.templateId,
      found->second.definition, found->second.vm);
  if (!staged.ok()) {
    clearSurfaceGlobal();
    found->second.vm.reset();
    failInitialization(dispatch, "page", "initialEvaluation",
                       staged.error().code, staged.error().message);
    return;
  }
  if (!callOptionalHook(found->second.vm, "onReady", contextValue)) {
    clearSurfaceGlobal();
    pageInitializationStage_.cancelOnExecutor(*dispatch.surfaceId);
    found->second.vm.reset();
    failInitialization(dispatch, "page", "onReady", "JS_EXCEPTION", "onReady failed");
    return;
  }
  clearSurfaceGlobal();
  static_cast<void>(engine_->drainMicrotasks(*context_, 16));
  RuntimeValue::Object complete{
      {"schemaVersion", RuntimeValue(1.0)},
      {"kind", RuntimeValue("vmInitializationResult")},
      {"requestId", RuntimeValue(dispatch.requestId)},
      {"scope", RuntimeValue("page")},
      {"status", RuntimeValue("completed")},
      {"surfaceId", RuntimeValue(*dispatch.surfaceId)}};
  const auto completionAccepted = postVmCompletion(RuntimeValue(complete));
  if (!completionAccepted) {
    androidVmFailure("completeVmInitialization", "Runtime ABI completion rejected");
    pageInitializationStage_.cancelOnExecutor(*dispatch.surfaceId);
    found->second.vm.reset();
    return;
  }
  auto submitted =
      pageInitializationStage_.submitInitialOnExecutor(*dispatch.surfaceId);
  if (!submitted.ok()) {
    androidVmFailure(submitted.error().code, submitted.error().message);
    pageInitializationStage_.cancelOnExecutor(*dispatch.surfaceId);
    found->second.vm.reset();
  }
}

void VmLifecycleService::onVmInitialization(
    const abi::VmInitializationDispatch &dispatch) noexcept {
  if (!onExecutor() || !running_) return;
  if (dispatch.scope == "app") initializeApp(dispatch);
  else if (dispatch.scope == "page") initializePage(dispatch);
}

void VmLifecycleService::closeSurfaceOnExecutor(std::string_view surfaceId) noexcept {
  if (!onExecutor()) return;
  pageInitializationStage_.cancelOnExecutor(surfaceId);
  pages_.erase(std::string(surfaceId));
  static_cast<void>(moduleLoader_.closeSurfaceOnExecutor(surfaceId));
}

void VmLifecycleService::stopOnExecutor() noexcept {
  if (!onExecutor()) return;
  pageInitializationStage_.cancelAllOnExecutor();
  pages_.clear();
  appVm_.reset();
  hasAppContext_ = false;
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

VmResourceSnapshot VmLifecycleService::resources() const noexcept {
  return {appVm_.valid() ? 1U : 0U, pages_.size(), pages_.size()};
}

} // namespace quickapp::js::vm
