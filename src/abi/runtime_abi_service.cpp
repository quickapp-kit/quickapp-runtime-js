#include "quickapp/js/abi/runtime_abi_service.h"

#include <algorithm>
#include <array>
#include <utility>

namespace quickapp::js::abi {
namespace {

struct NativeEntry {
  std::string_view name;
  CoreMessageKind kind;
};

constexpr std::array<NativeEntry, 16> kMessageEntries{{
    {"$quickapp_runtime_v1_instantiateTemplate$",
     CoreMessageKind::InstantiateTemplate},
    {"$quickapp_runtime_v1_completeVerifiedModuleLoad$",
     CoreMessageKind::CompleteVerifiedModuleLoad},
    {"$quickapp_runtime_v1_completeVmInitialization$",
     CoreMessageKind::CompleteVmInitialization},
    {"$quickapp_runtime_v1_submitRenderTransaction$",
     CoreMessageKind::SubmitRenderTransaction},
    {"$quickapp_runtime_v1_registerHandler$", CoreMessageKind::RegisterHandler},
    {"$quickapp_runtime_v1_unregisterHandler$",
     CoreMessageKind::UnregisterHandler},
    {"$quickapp_runtime_v1_pushRoute$", CoreMessageKind::NavigationPush},
    {"$quickapp_runtime_v1_closeRoute$", CoreMessageKind::NavigationClose},
    {"$quickapp_runtime_v1_showToast$", CoreMessageKind::ShowToast},
    {"$quickapp_runtime_v1_featureRequest$", CoreMessageKind::FeatureRequest},
    {"$quickapp_runtime_v1_getDeviceInfo$", CoreMessageKind::DeviceGetInfo},
    {"$quickapp_runtime_v1_startTimer$", CoreMessageKind::TimerStart},
    {"$quickapp_runtime_v1_cancelTimer$", CoreMessageKind::TimerCancel},
    {"$quickapp_runtime_v1_setTitleBar$", CoreMessageKind::SetTitleBar},
    {"$quickapp_runtime_v1_setMeta$", CoreMessageKind::SetMeta},
    {"$quickapp_runtime_v1_completeLifecycle$",
     CoreMessageKind::CompleteLifecycle},
}};

AbiRuntimeError error(AbiErrorCode code, std::string message,
                      bool retryable = false) {
  return {code, std::move(message), retryable, std::nullopt, std::nullopt,
          std::nullopt, std::nullopt};
}

RuntimeError engineError(const EngineException &exception) {
  return {exception.kind == EngineExceptionKind::OutOfMemory
              ? RuntimeErrorCode::OutOfMemory
              : RuntimeErrorCode::JsException,
          exception.message};
}

bool validJsRequestId(std::string_view value) noexcept {
  constexpr std::string_view prefix = "req:j-";
  if (!value.starts_with(prefix)) return false;
  const auto suffix = value.substr(prefix.size());
  if (suffix.empty() || suffix.front() == '0') return false;
  return std::all_of(suffix.begin(), suffix.end(),
                     [](const char character) {
                       return character >= '0' && character <= '9';
                     });
}

} // namespace

RuntimeAbiService::RuntimeAbiService(
    JsEngineService &engineService, CoreIngressPort &corePort,
    RuntimeAbiLimits limits, CapabilitySupportSnapshot capabilitySnapshot)
    : engineService_(engineService), corePort_(corePort), limits_(limits),
      capabilitySnapshot_(std::move(capabilitySnapshot)) {}

RuntimeAbiService::~RuntimeAbiService() {
  const auto current = state();
  if (current != RuntimeAbiServiceState::New &&
      current != RuntimeAbiServiceState::Stopped) {
    std::terminate();
  }
}

AbiServiceResult RuntimeAbiService::startOnExecutor(
    JsEnginePort &engine, const JsContextRef &context,
    std::string_view runtimeAbiIdentity) noexcept {
  try {
    if (!onExecutor()) {
      return AbiServiceResult::failure(
          error(AbiErrorCode::InvalidArgument, "wrong executor"));
    }
    RuntimeAbiServiceState expected = RuntimeAbiServiceState::New;
    if (!state_.compare_exchange_strong(expected,
                                        RuntimeAbiServiceState::Starting,
                                        std::memory_order_acq_rel)) {
      return AbiServiceResult::failure(
          error(AbiErrorCode::InvalidArgument, "invalid ABI start state"));
    }
    if (runtimeAbiIdentity != kRuntimeAbiIdentity) {
      state_.store(RuntimeAbiServiceState::Stopped, std::memory_order_release);
      return AbiServiceResult::failure(error(
          AbiErrorCode::UnsupportedVersion, "unsupported Runtime ABI identity"));
    }
    if (limits_.maxBridgeCorrelations == 0 ||
        limits_.valueLimits.maxDepth == 0 || limits_.valueLimits.maxNodes == 0) {
      state_.store(RuntimeAbiServiceState::Stopped, std::memory_order_release);
      return AbiServiceResult::failure(
          error(AbiErrorCode::InvalidArgument, "invalid Runtime ABI limits"));
    }

    engine_ = &engine;
    context_ = &context;
    bindingTokens_.reserve(kMessageEntries.size() + 1);
    for (const auto &entry : kMessageEntries) {
      NativeFunctionSpec spec{
          .globalName = std::string(entry.name),
          .minArgs = 1,
          .maxArgs = 1,
          .invoke = [this, kind = entry.kind](const NativeCallView &call) {
            return invokeNative(kind, call);
          },
      };
      auto token = engine_->bindNativeFunction(context, spec);
      if (!token.ok()) {
        unbindAllOnExecutor();
        engine_ = nullptr;
        context_ = nullptr;
        state_.store(RuntimeAbiServiceState::Stopped,
                     std::memory_order_release);
        return AbiServiceResult::failure(error(
            token.error().kind == EngineExceptionKind::OutOfMemory
                ? AbiErrorCode::OutOfMemory
                : AbiErrorCode::JsException,
            token.error().message));
      }
      bindingTokens_.push_back(std::move(token).value());
    }

    NativeFunctionSpec supportSpec{
        .globalName = "$quickapp_runtime_v1_supportsCapability$",
        .minArgs = 2,
        .maxArgs = 2,
        .invoke = [this](const NativeCallView &call) {
          return invokeSupportsCapability(call);
        },
    };
    auto supportToken = engine_->bindNativeFunction(context, supportSpec);
    if (!supportToken.ok()) {
      unbindAllOnExecutor();
      engine_ = nullptr;
      context_ = nullptr;
      state_.store(RuntimeAbiServiceState::Stopped, std::memory_order_release);
      return AbiServiceResult::failure(error(
          supportToken.error().kind == EngineExceptionKind::OutOfMemory
              ? AbiErrorCode::OutOfMemory
              : AbiErrorCode::JsException,
          supportToken.error().message));
    }
    bindingTokens_.push_back(std::move(supportToken).value());
    state_.store(RuntimeAbiServiceState::Running, std::memory_order_release);
    return AbiServiceResult::success();
  } catch (const std::bad_alloc &) {
    unbindAllOnExecutor();
    engine_ = nullptr;
    context_ = nullptr;
    state_.store(RuntimeAbiServiceState::Stopped, std::memory_order_release);
    return AbiServiceResult::failure(
        error(AbiErrorCode::OutOfMemory, "out of memory", true));
  } catch (...) {
    unbindAllOnExecutor();
    engine_ = nullptr;
    context_ = nullptr;
    state_.store(RuntimeAbiServiceState::Stopped, std::memory_order_release);
    return AbiServiceResult::failure(
        error(AbiErrorCode::JsException, "Runtime ABI start failed"));
  }
}

AbiServiceResult
RuntimeAbiService::openSurfaceOnExecutor(std::string surfaceId) noexcept {
  if (!onExecutor() || state() != RuntimeAbiServiceState::Running ||
      !surfaceId.starts_with("srf:") || surfaceId.size() <= 4) {
    return AbiServiceResult::failure(
        error(AbiErrorCode::InvalidArgument, "invalid Surface open"));
  }
  try {
    std::lock_guard lock(surfaceMutex_);
    if (surfaces_.contains(surfaceId)) {
      return AbiServiceResult::failure(
          error(AbiErrorCode::InvalidArgument, "Surface already exists"));
    }
    surfaces_.emplace(std::move(surfaceId), SurfaceScope{});
    return AbiServiceResult::success();
  } catch (const std::bad_alloc &) {
    return AbiServiceResult::failure(
        error(AbiErrorCode::OutOfMemory, "out of memory", true));
  }
}

AbiServiceResult RuntimeAbiService::closeSurfaceOnExecutor(
    std::string_view surfaceId) noexcept {
  if (!onExecutor()) {
    return AbiServiceResult::failure(
        error(AbiErrorCode::InvalidArgument, "wrong executor"));
  }
  {
    std::lock_guard lock(surfaceMutex_);
    const auto found = surfaces_.find(surfaceId);
    if (found == surfaces_.end()) {
      return AbiServiceResult::success();
    }
    found->second.open = false;
    ++found->second.generation;
  }
  std::erase_if(correlations_, [&](const auto &entry) {
    return entry.second.owner.kind == CorrelationOwnerKind::Surface &&
           entry.second.owner.surfaceId == surfaceId;
  });
  return AbiServiceResult::success();
}

ConsumerRegistrationResult
RuntimeAbiService::registerConsumersOnExecutor(CallbackSlots slots) noexcept {
  if (!onExecutor() || state() != RuntimeAbiServiceState::Running ||
      activeConsumerRegistration_ != 0) {
    return ConsumerRegistrationResult::failure(
        error(AbiErrorCode::InvalidArgument,
              "consumer registration is not available"));
  }
  try {
    callbackSlots_ = std::move(slots);
    activeConsumerRegistration_ = ++nextConsumerRegistration_;
    return ConsumerRegistrationResult::success(
        ConsumerRegistrationToken(activeConsumerRegistration_));
  } catch (const std::bad_alloc &) {
    return ConsumerRegistrationResult::failure(
        error(AbiErrorCode::OutOfMemory, "out of memory", true));
  }
}

bool RuntimeAbiService::unregisterConsumersOnExecutor(
    ConsumerRegistrationToken &token) noexcept {
  if (!onExecutor() || !token.valid() ||
      token.id_ != activeConsumerRegistration_) {
    return false;
  }
  callbackSlots_ = CallbackSlots{};
  activeConsumerRegistration_ = 0;
  token.id_ = 0;
  return true;
}

EnqueueResult
RuntimeAbiService::postCallback(JsInboundMessage message) noexcept {
  try {
    if (state() != RuntimeAbiServiceState::Running) {
      return EnqueueResult::rejected(
          error(AbiErrorCode::PortClosed, "Runtime ABI is closed"));
    }
    auto validation = validateJsInboundMessage(message, limits_.valueLimits);
    if (!validation.ok()) {
      return EnqueueResult::rejected(validation.error());
    }
    const auto traceKey = callbackCorrelation(message);
    const auto traceSurface = callbackSurfaceId(message);
    const auto generation = admittedGeneration(message);
    if (!generation) {
      return EnqueueResult::rejected(
          error(AbiErrorCode::SurfaceNotFound, "Surface is not open"));
    }

    auto self = shared_from_this();
    queuedCallbacks_.fetch_add(1, std::memory_order_acq_rel);
    const auto posted = engineService_.post(
        [self, message = std::move(message), generation = *generation](
            JsEnginePort &, const JsContextRef &) mutable {
          self->dispatchCallbackOnExecutor(std::move(message), generation);
          self->queuedCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
        },
        [self] {
          self->queuedCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
        });
    if (posted.status != PostStatus::Accepted) {
      queuedCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
      if (traceKey) {
        observeBridge("bridge.request.failed", *traceKey, traceSurface,
                      "QUEUE_OVERFLOW");
      }
      return EnqueueResult::rejected(error(
          posted.status == PostStatus::QueueOverflow
              ? AbiErrorCode::QueueOverflow
              : AbiErrorCode::PortClosed,
          posted.status == PostStatus::QueueOverflow ? "JS callback queue full"
                                                     : "Runtime ABI is closing",
          posted.status == PostStatus::QueueOverflow));
    }
    return EnqueueResult::accepted();
  } catch (const std::bad_weak_ptr &) {
    return EnqueueResult::rejected(
        error(AbiErrorCode::PortClosed, "Runtime ABI owner is unavailable"));
  } catch (const std::bad_alloc &) {
    return EnqueueResult::rejected(
        error(AbiErrorCode::OutOfMemory, "out of memory", true));
  } catch (...) {
    return EnqueueResult::rejected(
        error(AbiErrorCode::JsException, "callback admission failed"));
  }
}

EnqueueResult RuntimeAbiService::submitPageControlOnExecutor(
    SetTitleBar message) noexcept {
  if (!onExecutor() || state() != RuntimeAbiServiceState::Running ||
      !validJsRequestId(message.requestId) ||
      !message.surfaceId.starts_with("srf:") ||
      message.surfaceId.size() <= 4 || message.text.empty()) {
    return EnqueueResult::rejected(
        error(AbiErrorCode::InvalidArgument, "invalid SetTitleBar message"));
  }
  return submitCoreMessage(CoreInboundMessage(std::move(message)));
}

EnqueueResult
RuntimeAbiService::submitPageControlOnExecutor(SetMeta message) noexcept {
  if (!onExecutor() || state() != RuntimeAbiServiceState::Running ||
      !validJsRequestId(message.requestId) ||
      !message.surfaceId.starts_with("srf:") ||
      message.surfaceId.size() <= 4 || (!message.title && !message.description)) {
    return EnqueueResult::rejected(
        error(AbiErrorCode::InvalidArgument, "invalid SetMeta message"));
  }
  return submitCoreMessage(CoreInboundMessage(std::move(message)));
}

void RuntimeAbiService::stopOnExecutor() noexcept {
  if (!onExecutor()) {
    return;
  }
  const auto current = state();
  if (current == RuntimeAbiServiceState::Stopped) {
    return;
  }
  state_.store(RuntimeAbiServiceState::Quiescing, std::memory_order_release);
  unbindAllOnExecutor();
  correlations_.clear();
  callbackSlots_ = CallbackSlots{};
  activeConsumerRegistration_ = 0;
  {
    std::lock_guard lock(surfaceMutex_);
    for (auto &[_, surface] : surfaces_) {
      surface.open = false;
      ++surface.generation;
    }
    surfaces_.clear();
  }
  engine_ = nullptr;
  context_ = nullptr;
  state_.store(RuntimeAbiServiceState::Stopped, std::memory_order_release);
}

RuntimeAbiServiceState RuntimeAbiService::state() const noexcept {
  return state_.load(std::memory_order_acquire);
}

RuntimeAbiResourceSnapshot RuntimeAbiService::resources() const noexcept {
  std::size_t openSurfaces = 0;
  {
    std::lock_guard lock(surfaceMutex_);
    for (const auto &[_, surface] : surfaces_) {
      openSurfaces += surface.open ? 1U : 0U;
    }
  }
  return {bindingTokens_.size(), correlations_.size(),
          activeConsumerRegistration_ == 0 ? 0U : 1U, openSurfaces,
          queuedCallbacks_.load(std::memory_order_acquire)};
}

NativeFunctionResult RuntimeAbiService::invokeNative(
    CoreMessageKind kind, const NativeCallView &call) noexcept {
  try {
    if (!onExecutor() || state() != RuntimeAbiServiceState::Running ||
        !engine_ || !context_ || call.args.size() != 1) {
      return encodeNativeResult(EnqueueResult::rejected(
          error(AbiErrorCode::PortClosed, "Runtime ABI is not running")));
    }
    auto retained = engine_->retain(call.context, call.args.front());
    if (!retained.ok()) {
      return NativeFunctionResult::failure(engineError(retained.error()));
    }
    auto valueRef = std::move(retained).value();
    auto converted =
        engine_->toRuntimeValue(call.context, valueRef, limits_.valueLimits);
    valueRef.reset();
    if (!converted.ok()) {
      return encodeNativeResult(EnqueueResult::rejected(error(
          converted.error().kind == EngineExceptionKind::OutOfMemory
              ? AbiErrorCode::OutOfMemory
              : AbiErrorCode::InvalidArgument,
          converted.error().message,
          converted.error().kind == EngineExceptionKind::OutOfMemory)));
    }
    auto decoded = decodeCoreMessage(kind, converted.value(), limits_.valueLimits);
    if (!decoded.ok()) {
      return encodeNativeResult(EnqueueResult::rejected(decoded.error()));
    }
    return encodeNativeResult(submitCoreMessage(std::move(decoded).value()));
  } catch (const std::bad_alloc &) {
    return encodeNativeResult(EnqueueResult::rejected(
        error(AbiErrorCode::OutOfMemory, "out of memory", true)));
  } catch (...) {
    return NativeFunctionResult::failure(
        {RuntimeErrorCode::JsException, "Runtime ABI native entry failed"});
  }
}

NativeFunctionResult RuntimeAbiService::invokeSupportsCapability(
    const NativeCallView &call) noexcept {
  try {
    if (!onExecutor() || state() != RuntimeAbiServiceState::Running ||
        !engine_ || call.args.size() != 2) {
      return NativeFunctionResult::failure(
          {RuntimeErrorCode::JsException, "invalid capability query"});
    }
    std::array<std::string, 2> values;
    for (std::size_t index = 0; index < call.args.size(); ++index) {
      auto retained = engine_->retain(call.context, call.args[index]);
      if (!retained.ok()) {
        return NativeFunctionResult::failure(engineError(retained.error()));
      }
      auto ref = std::move(retained).value();
      auto converted =
          engine_->toRuntimeValue(call.context, ref, limits_.valueLimits);
      ref.reset();
      if (!converted.ok()) {
        return NativeFunctionResult::failure(engineError(converted.error()));
      }
      const auto *text = std::get_if<std::string>(&converted.value().storage());
      if (!text) {
        return NativeFunctionResult::failure(
            {RuntimeErrorCode::AbiInvalidArgument,
             "capability query arguments must be strings"});
      }
      values[index] = *text;
    }
    auto result = engine_->fromRuntimeValue(
        call.context,
        RuntimeValue(capabilitySnapshot_.supports(values[0], values[1])));
    if (!result.ok()) {
      return NativeFunctionResult::failure(engineError(result.error()));
    }
    return NativeFunctionResult::success(std::move(result).value());
  } catch (const std::bad_alloc &) {
    return NativeFunctionResult::failure(
        {RuntimeErrorCode::OutOfMemory, "out of memory"});
  } catch (...) {
    return NativeFunctionResult::failure(
        {RuntimeErrorCode::JsException, "capability query failed"});
  }
}

NativeFunctionResult
RuntimeAbiService::encodeNativeResult(const EnqueueResult &result) noexcept {
  try {
    if (!engine_ || !context_) {
      return NativeFunctionResult::failure(
          {RuntimeErrorCode::JsException, "Runtime ABI engine is unavailable"});
    }
    auto encoded = engine_->fromRuntimeValue(*context_, encodeEnqueueResult(result));
    if (!encoded.ok()) {
      return NativeFunctionResult::failure(engineError(encoded.error()));
    }
    return NativeFunctionResult::success(std::move(encoded).value());
  } catch (const std::bad_alloc &) {
    return NativeFunctionResult::failure(
        {RuntimeErrorCode::OutOfMemory, "out of memory"});
  } catch (...) {
    return NativeFunctionResult::failure(
        {RuntimeErrorCode::JsException, "failed to encode EnqueueResult"});
  }
}

EnqueueResult
RuntimeAbiService::submitCoreMessage(CoreInboundMessage message) noexcept {
  try {
    const auto surfaceId = coreSurfaceId(message);
    std::uint64_t generation = 1;
    if (surfaceId) {
      std::lock_guard lock(surfaceMutex_);
      const auto surface = surfaces_.find(*surfaceId);
      if (surface == surfaces_.end() || !surface->second.open) {
        auto result = EnqueueResult::rejected(
            error(AbiErrorCode::SurfaceNotFound, "Surface is not open"));
        result.error->surfaceId = *surfaceId;
        return result;
      }
      generation = surface->second.generation;
    }

    std::optional<CorrelationKey> provisionalKey;
    if (coreMessageNeedsCorrelation(message)) {
      if (correlations_.size() >= limits_.maxBridgeCorrelations) {
        auto result = EnqueueResult::rejected(error(
            AbiErrorCode::QueueOverflow, "bridge correlation capacity reached",
            true));
        if (const auto key = coreCorrelation(message); key) {
          observeBridge("bridge.request.failed", *key, surfaceId,
                        "QUEUE_OVERFLOW");
        }
        return result;
      }
      const auto key = coreCorrelation(message);
      const auto expected = expectedResultKind(message);
      if (!key || !expected || correlations_.contains(*key)) {
        auto result = EnqueueResult::rejected(
            error(AbiErrorCode::InvalidArgument, "duplicate correlation key"));
        if (key) {
          observeBridge("bridge.request.failed", *key, surfaceId,
                        "ABI_INVALID_ARGUMENT");
        }
        return result;
      }
      PendingRecord record{
          .key = *key,
          .expectedResultKind = *expected,
          .owner = surfaceId
                       ? CorrelationOwner{CorrelationOwnerKind::Surface,
                                          *surfaceId}
                       : CorrelationOwner{CorrelationOwnerKind::AppRuntime, {}},
          .ownerGeneration = generation,
      };
      correlations_.emplace(record.key, record);
      provisionalKey = record.key;
    }

    auto result = corePort_.post(std::move(message));
    if (!result.ok && provisionalKey) {
      correlations_.erase(*provisionalKey);
    }
    if (provisionalKey) {
      if (result.ok) {
        observeBridge("bridge.request.enqueued", *provisionalKey, surfaceId);
      } else {
        observeBridge("bridge.request.failed", *provisionalKey, surfaceId,
                      result.error
                          ? std::optional<std::string_view>(
                                abiErrorCodeName(result.error->code))
                          : std::optional<std::string_view>("PLATFORM_REJECTED"));
      }
    }
    return result;
  } catch (const std::bad_alloc &) {
    return EnqueueResult::rejected(
        error(AbiErrorCode::OutOfMemory, "out of memory", true));
  } catch (...) {
    return EnqueueResult::rejected(
        error(AbiErrorCode::PortClosed, "Core ingress failed"));
  }
}

void RuntimeAbiService::dispatchCallbackOnExecutor(
    JsInboundMessage message, std::uint64_t admittedGeneration) noexcept {
  if (!onExecutor() || state() != RuntimeAbiServiceState::Running) {
    return;
  }
  const auto surfaceId = callbackSurfaceId(message);
  if (surfaceId) {
    std::lock_guard lock(surfaceMutex_);
    const auto surface = surfaces_.find(*surfaceId);
    if (surface == surfaces_.end() || !surface->second.open ||
        surface->second.generation != admittedGeneration) {
      return;
    }
  }

  if (callbackIsResult(message)) {
    const auto key = callbackCorrelation(message);
    if (!key) {
      return;
    }
    const auto pending = correlations_.find(*key);
    if (pending == correlations_.end() ||
        pending->second.expectedResultKind != callbackKind(message) ||
        pending->second.ownerGeneration != admittedGeneration ||
        (pending->second.owner.kind == CorrelationOwnerKind::Surface &&
         (!surfaceId || pending->second.owner.surfaceId != *surfaceId))) {
      observeBridge("bridge.request.failed", *key, surfaceId,
                    "ABI_INVALID_ARGUMENT");
      return;
    }
    correlations_.erase(pending);
    observeBridge("bridge.request.completed", *key, surfaceId);
  }
  dispatchToConsumer(message);
}

void RuntimeAbiService::dispatchToConsumer(
    const JsInboundMessage &message) noexcept {
  if (activeConsumerRegistration_ == 0) {
    return;
  }
  std::visit(
      [this](const auto &typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, LoadVerifiedModule>) {
          if (callbackSlots_.loadVerifiedModule)
            callbackSlots_.loadVerifiedModule(typed);
        } else if constexpr (std::is_same_v<T, AppContext>) {
          if (callbackSlots_.appContext)
            callbackSlots_.appContext(typed);
        } else if constexpr (std::is_same_v<T, SurfaceContext>) {
          if (callbackSlots_.surfaceContext)
            callbackSlots_.surfaceContext(typed);
        } else if constexpr (std::is_same_v<T, VmInitializationDispatch>) {
          if (callbackSlots_.vmInitializationDispatch)
            callbackSlots_.vmInitializationDispatch(typed);
        } else if constexpr (std::is_same_v<T, LifecycleDispatch>) {
          if (callbackSlots_.lifecycleDispatch)
            callbackSlots_.lifecycleDispatch(typed);
        } else if constexpr (std::is_same_v<T, JsEventDispatch>) {
          if (callbackSlots_.jsEventDispatch)
            callbackSlots_.jsEventDispatch(typed);
        } else if constexpr (std::is_same_v<T, InstantiateTemplateResult>) {
          if (callbackSlots_.instantiateTemplateResult)
            callbackSlots_.instantiateTemplateResult(typed);
        } else if constexpr (std::is_same_v<T, HandlerRegistrationResult>) {
          if (callbackSlots_.handlerRegistrationResult)
            callbackSlots_.handlerRegistrationResult(typed);
        } else if constexpr (std::is_same_v<T, RenderTransactionResult>) {
          if (callbackSlots_.renderTransactionResult)
            callbackSlots_.renderTransactionResult(typed);
        } else if constexpr (std::is_same_v<T, NavigationPushResult>) {
          if (callbackSlots_.navigationPushResult)
            callbackSlots_.navigationPushResult(typed);
        } else if constexpr (std::is_same_v<T, NavigationCloseResult>) {
          if (callbackSlots_.navigationCloseResult)
            callbackSlots_.navigationCloseResult(typed);
        } else if constexpr (std::is_same_v<T, ShowToastResult>) {
          if (callbackSlots_.showToastResult)
            callbackSlots_.showToastResult(typed);
        } else if constexpr (std::is_same_v<T, FeatureResult>) {
          if (callbackSlots_.featureResult)
            callbackSlots_.featureResult(typed);
        } else if constexpr (std::is_same_v<T, DeviceGetInfoResult>) {
          if (callbackSlots_.deviceGetInfoResult)
            callbackSlots_.deviceGetInfoResult(typed);
        } else if constexpr (std::is_same_v<T, TimerStartResult>) {
          if (callbackSlots_.timerStartResult)
            callbackSlots_.timerStartResult(typed);
        } else if constexpr (std::is_same_v<T, TimerCancelResult>) {
          if (callbackSlots_.timerCancelResult)
            callbackSlots_.timerCancelResult(typed);
        } else if constexpr (std::is_same_v<T, TimerFired>) {
          if (callbackSlots_.timerFired)
            callbackSlots_.timerFired(typed);
        } else if constexpr (std::is_same_v<T, SetTitleBarResult>) {
          if (callbackSlots_.setTitleBarResult)
            callbackSlots_.setTitleBarResult(typed);
        } else if constexpr (std::is_same_v<T, SetMetaResult>) {
          if (callbackSlots_.setMetaResult)
            callbackSlots_.setMetaResult(typed);
        } else if constexpr (std::is_same_v<T, SurfaceStatusChanged>) {
          if (callbackSlots_.surfaceStatusChanged)
            callbackSlots_.surfaceStatusChanged(typed);
        }
      },
      message);
}

void RuntimeAbiService::observeBridge(
    std::string_view markerName, const CorrelationKey &key,
    const std::optional<std::string> &surfaceId,
    std::optional<std::string_view> errorCode) noexcept {
  if (key.kind != CorrelationKeyKind::Request) {
    return;
  }
  static_cast<void>(engineService_.observation().emitBridge(
      markerName, engineService_.appRuntimeId(), key.value,
      surfaceId ? std::optional<std::string_view>(*surfaceId) : std::nullopt,
      errorCode));
}

void RuntimeAbiService::unbindAllOnExecutor() noexcept {
  if (!engine_ || !context_) {
    bindingTokens_.clear();
    return;
  }
  for (auto token = bindingTokens_.rbegin(); token != bindingTokens_.rend();
       ++token) {
    if (token->valid()) {
      static_cast<void>(engine_->unbindNativeFunction(*context_, *token));
    }
  }
  bindingTokens_.clear();
}

std::optional<std::uint64_t>
RuntimeAbiService::admittedGeneration(
    const JsInboundMessage &message) const noexcept {
  const auto surfaceId = callbackSurfaceId(message);
  if (!surfaceId) {
    return 1;
  }
  std::lock_guard lock(surfaceMutex_);
  const auto surface = surfaces_.find(*surfaceId);
  if (surface == surfaces_.end() || !surface->second.open) {
    return std::nullopt;
  }
  return surface->second.generation;
}

bool RuntimeAbiService::onExecutor() const noexcept {
  return engineService_.executor().isOnExecutor();
}

} // namespace quickapp::js::abi
