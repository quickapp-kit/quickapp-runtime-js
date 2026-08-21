#include "quickapp/js/page/page_host_control.h"

#include <array>
#include <exception>
#include <new>
#include <utility>

namespace quickapp::js::page {
namespace {

constexpr std::string_view kSetTitleBarEntry =
    "$quickapp_framework_v1_setTitleBar$";
constexpr std::string_view kSetMetaEntry = "$quickapp_framework_v1_setMeta$";
constexpr std::string_view kFacadeFactorySource = R"JS(
(function(surfaceId, hostCapabilities) {
  const page = {};
  if (hostCapabilities.indexOf("setTitleBar") !== -1) {
    page.setTitleBar = function(options) {
      return $quickapp_framework_v1_setTitleBar$(surfaceId, options);
    };
  }
  if (hostCapabilities.indexOf("setMeta") !== -1) {
    page.setMeta = function(options) {
      return $quickapp_framework_v1_setMeta$(surfaceId, options);
    };
  }
  return Object.freeze(page);
})
)JS";

RuntimeError nativeFailure(RuntimeErrorCode code, std::string message) {
  return {code, std::move(message)};
}

const std::string *text(const RuntimeValue::Object &object,
                        std::string_view key) {
  const auto found = object.find(key);
  return found == object.end()
             ? nullptr
             : std::get_if<std::string>(&found->second.storage());
}

vm::PageVmSetupError setupFailure(std::string message) {
  return {"JS_EXCEPTION", std::move(message)};
}

} // namespace

PageHostControlInstaller::PageHostControlInstaller(
    JsEngineService &engineService, abi::RuntimeAbiService &runtimeAbi,
    framework::JsRequestIdAllocatorPort &requestIds)
    : engineService_(engineService), runtimeAbi_(runtimeAbi),
      requestIds_(requestIds) {}

PageHostControlInstaller::~PageHostControlInstaller() {
  if (running_)
    std::terminate();
}

bool PageHostControlInstaller::onExecutor() const noexcept {
  return engineService_.executor().isOnExecutor();
}

bool PageHostControlInstaller::startOnExecutor(
    JsEnginePort &engine, const JsContextRef &context) noexcept {
  if (!onExecutor() || running_ || !context.valid())
    return false;
  try {
    engine_ = &engine;
    context_ = &context;
    bindings_.reserve(2);

    NativeFunctionSpec titleSpec{
        .globalName = std::string(kSetTitleBarEntry),
        .minArgs = 2,
        .maxArgs = 2,
        .invoke =
            [this](const NativeCallView &call) {
              return invokeSetTitleBar(call);
            },
    };
    auto titleToken = engine_->bindNativeFunction(context, titleSpec);
    if (!titleToken.ok()) {
      engine_ = nullptr;
      context_ = nullptr;
      return false;
    }
    bindings_.push_back(std::move(titleToken).value());

    NativeFunctionSpec metaSpec{
        .globalName = std::string(kSetMetaEntry),
        .minArgs = 2,
        .maxArgs = 2,
        .invoke =
            [this](const NativeCallView &call) { return invokeSetMeta(call); },
    };
    auto metaToken = engine_->bindNativeFunction(context, metaSpec);
    if (!metaToken.ok()) {
      unbindAllOnExecutor();
      engine_ = nullptr;
      context_ = nullptr;
      return false;
    }
    bindings_.push_back(std::move(metaToken).value());

    SourceUnit factorySource{
        "alpha-page-host-control", "quickapp://framework/page-host-control",
        std::string(kFacadeFactorySource), SourceMode::Script};
    auto factory = engine_->evaluate(context, factorySource);
    if (!factory.ok()) {
      unbindAllOnExecutor();
      engine_ = nullptr;
      context_ = nullptr;
      return false;
    }
    facadeFactory_ = std::move(factory).value();
    running_ = true;
    return true;
  } catch (...) {
    unbindAllOnExecutor();
    facadeFactory_.reset();
    engine_ = nullptr;
    context_ = nullptr;
    return false;
  }
}

vm::PageVmSetupResult PageHostControlInstaller::installOnExecutor(
    JsValueRef &pageVm, const abi::SurfaceContext &surface) noexcept {
  if (!onExecutor() || !running_ || !engine_ || !context_ || !pageVm.valid()) {
    return vm::PageVmSetupResult::failure(
        setupFailure("Page host control installer is unavailable"));
  }
  try {
    RuntimeValue::Array capabilities;
    capabilities.reserve(surface.hostCapabilities.size());
    for (const auto &capability : surface.hostCapabilities) {
      if (capability == "setTitleBar" || capability == "setMeta") {
        capabilities.emplace_back(capability);
      }
    }
    auto surfaceId =
        engine_->fromRuntimeValue(*context_, RuntimeValue(surface.surfaceId));
    auto capabilityValue = engine_->fromRuntimeValue(
        *context_, RuntimeValue(std::move(capabilities)));
    auto null = engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
    if (!surfaceId.ok() || !capabilityValue.ok() || !null.ok()) {
      return vm::PageVmSetupResult::failure(
          setupFailure("Page host control arguments could not be created"));
    }
    std::array<JsValueRef, 2> args{std::move(surfaceId).value(),
                                   std::move(capabilityValue).value()};
    auto facade =
        engine_->call(*context_, facadeFactory_, std::move(null).value(),
                      std::span<const JsValueRef>(args.data(), args.size()));
    if (!facade.ok()) {
      return vm::PageVmSetupResult::failure(
          setupFailure(facade.error().message));
    }
    auto assigned =
        engine_->setProperty(*context_, pageVm, "$page", facade.value());
    if (!assigned.ok()) {
      return vm::PageVmSetupResult::failure(
          setupFailure(assigned.error().message));
    }
    return vm::PageVmSetupResult::success();
  } catch (const std::bad_alloc &) {
    return vm::PageVmSetupResult::failure(
        {"OUT_OF_MEMORY", "Page host control allocation failed"});
  } catch (...) {
    return vm::PageVmSetupResult::failure(
        setupFailure("Page host control installation failed"));
  }
}

NativeFunctionResult PageHostControlInstaller::invokeSetTitleBar(
    const NativeCallView &call) noexcept {
  try {
    if (!onExecutor() || !running_ || !engine_ || call.args.size() != 2) {
      return NativeFunctionResult::failure(nativeFailure(
          RuntimeErrorCode::AbiInvalidArgument, "invalid setTitleBar call"));
    }
    auto surface = engine_->retain(call.context, call.args[0]);
    auto options = engine_->retain(call.context, call.args[1]);
    if (!surface.ok() || !options.ok()) {
      return NativeFunctionResult::failure(nativeFailure(
          RuntimeErrorCode::JsException, "setTitleBar arguments unavailable"));
    }
    auto surfaceValue =
        engine_->toRuntimeValue(call.context, surface.value(), {4, 8});
    auto optionValue =
        engine_->toRuntimeValue(call.context, options.value(), {4, 16});
    const auto *surfaceId =
        surfaceValue.ok()
            ? std::get_if<std::string>(&surfaceValue.value().storage())
            : nullptr;
    const auto *object =
        optionValue.ok()
            ? std::get_if<RuntimeValue::Object>(&optionValue.value().storage())
            : nullptr;
    const auto *title = object ? text(*object, "text") : nullptr;
    if (!surfaceId || !object || object->size() != 1 || !title ||
        title->empty()) {
      return NativeFunctionResult::failure(nativeFailure(
          RuntimeErrorCode::AbiInvalidArgument,
          "setTitleBar requires exactly one non-empty text field"));
    }
    return encodeAdmission(runtimeAbi_.submitPageControlOnExecutor(
        abi::SetTitleBar{requestIds_.nextRequestId(), *surfaceId, *title}));
  } catch (const std::bad_alloc &) {
    return NativeFunctionResult::failure(
        nativeFailure(RuntimeErrorCode::OutOfMemory, "out of memory"));
  } catch (...) {
    return NativeFunctionResult::failure(nativeFailure(
        RuntimeErrorCode::JsException, "setTitleBar adapter failed"));
  }
}

NativeFunctionResult
PageHostControlInstaller::invokeSetMeta(const NativeCallView &call) noexcept {
  try {
    if (!onExecutor() || !running_ || !engine_ || call.args.size() != 2) {
      return NativeFunctionResult::failure(nativeFailure(
          RuntimeErrorCode::AbiInvalidArgument, "invalid setMeta call"));
    }
    auto surface = engine_->retain(call.context, call.args[0]);
    auto options = engine_->retain(call.context, call.args[1]);
    if (!surface.ok() || !options.ok()) {
      return NativeFunctionResult::failure(nativeFailure(
          RuntimeErrorCode::JsException, "setMeta arguments unavailable"));
    }
    auto surfaceValue =
        engine_->toRuntimeValue(call.context, surface.value(), {4, 8});
    auto optionValue =
        engine_->toRuntimeValue(call.context, options.value(), {4, 16});
    const auto *surfaceId =
        surfaceValue.ok()
            ? std::get_if<std::string>(&surfaceValue.value().storage())
            : nullptr;
    const auto *object =
        optionValue.ok()
            ? std::get_if<RuntimeValue::Object>(&optionValue.value().storage())
            : nullptr;
    if (!surfaceId || !object || object->empty() || object->size() > 2) {
      return NativeFunctionResult::failure(
          nativeFailure(RuntimeErrorCode::AbiInvalidArgument,
                        "setMeta requires title and/or description"));
    }
    for (const auto &[key, value] : *object) {
      if ((key != "title" && key != "description") ||
          !std::get_if<std::string>(&value.storage())) {
        return NativeFunctionResult::failure(
            nativeFailure(RuntimeErrorCode::AbiInvalidArgument,
                          "setMeta accepts only string title and description"));
      }
    }
    const auto *title = text(*object, "title");
    const auto *description = text(*object, "description");
    return encodeAdmission(runtimeAbi_.submitPageControlOnExecutor(
        abi::SetMeta{requestIds_.nextRequestId(), *surfaceId,
                     title ? std::optional<std::string>(*title) : std::nullopt,
                     description ? std::optional<std::string>(*description)
                                 : std::nullopt}));
  } catch (const std::bad_alloc &) {
    return NativeFunctionResult::failure(
        nativeFailure(RuntimeErrorCode::OutOfMemory, "out of memory"));
  } catch (...) {
    return NativeFunctionResult::failure(
        nativeFailure(RuntimeErrorCode::JsException, "setMeta adapter failed"));
  }
}

NativeFunctionResult PageHostControlInstaller::encodeAdmission(
    const abi::EnqueueResult &result) noexcept {
  if (!engine_ || !context_) {
    return NativeFunctionResult::failure(nativeFailure(
        RuntimeErrorCode::JsException, "Page control engine is unavailable"));
  }
  auto encoded =
      engine_->fromRuntimeValue(*context_, abi::encodeEnqueueResult(result));
  if (!encoded.ok()) {
    return NativeFunctionResult::failure(
        nativeFailure(encoded.error().kind == EngineExceptionKind::OutOfMemory
                          ? RuntimeErrorCode::OutOfMemory
                          : RuntimeErrorCode::JsException,
                      encoded.error().message));
  }
  return NativeFunctionResult::success(std::move(encoded).value());
}

void PageHostControlInstaller::unbindAllOnExecutor() noexcept {
  if (!engine_ || !context_) {
    bindings_.clear();
    return;
  }
  for (auto iterator = bindings_.rbegin(); iterator != bindings_.rend();
       ++iterator) {
    static_cast<void>(engine_->unbindNativeFunction(*context_, *iterator));
  }
  bindings_.clear();
}

void PageHostControlInstaller::stopOnExecutor() noexcept {
  if (!onExecutor())
    return;
  facadeFactory_.reset();
  unbindAllOnExecutor();
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

PageHostControlResourceSnapshot
PageHostControlInstaller::resources() const noexcept {
  return {bindings_.size(), facadeFactory_.valid() ? 1U : 0U};
}

} // namespace quickapp::js::page
