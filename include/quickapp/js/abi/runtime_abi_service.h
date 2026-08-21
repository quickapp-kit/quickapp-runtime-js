#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "quickapp/js/abi/runtime_abi_codec.h"
#include "quickapp/js/engine/js_engine_service.h"

namespace quickapp::js::abi {

enum class RuntimeAbiServiceState {
  New,
  Starting,
  Running,
  Quiescing,
  Failed,
  Stopped,
};

using AbiServiceResult = Result<void, AbiRuntimeError>;
using ConsumerRegistrationResult =
    Result<ConsumerRegistrationToken, AbiRuntimeError>;

class RuntimeAbiService final
    : public std::enable_shared_from_this<RuntimeAbiService> {
public:
  RuntimeAbiService(JsEngineService &engineService, CoreIngressPort &corePort,
                    RuntimeAbiLimits limits,
                    CapabilitySupportSnapshot capabilitySnapshot);
  ~RuntimeAbiService();

  RuntimeAbiService(const RuntimeAbiService &) = delete;
  RuntimeAbiService &operator=(const RuntimeAbiService &) = delete;

  [[nodiscard]] AbiServiceResult
  startOnExecutor(JsEnginePort &engine, const JsContextRef &context,
                  std::string_view runtimeAbiIdentity) noexcept;
  [[nodiscard]] AbiServiceResult openSurfaceOnExecutor(
      std::string surfaceId) noexcept;
  [[nodiscard]] AbiServiceResult closeSurfaceOnExecutor(
      std::string_view surfaceId) noexcept;

  [[nodiscard]] ConsumerRegistrationResult
  registerConsumersOnExecutor(CallbackSlots slots) noexcept;
  [[nodiscard]] bool unregisterConsumersOnExecutor(
      ConsumerRegistrationToken &token) noexcept;

  [[nodiscard]] EnqueueResult postCallback(JsInboundMessage message) noexcept;
  [[nodiscard]] EnqueueResult
  submitPageControlOnExecutor(SetTitleBar message) noexcept;
  [[nodiscard]] EnqueueResult
  submitPageControlOnExecutor(SetMeta message) noexcept;
  void stopOnExecutor() noexcept;

  [[nodiscard]] RuntimeAbiServiceState state() const noexcept;
  [[nodiscard]] RuntimeAbiResourceSnapshot resources() const noexcept;

private:
  struct SurfaceScope {
    std::uint64_t generation{1};
    bool open{true};
  };

  [[nodiscard]] NativeFunctionResult
  invokeNative(CoreMessageKind kind, const NativeCallView &call) noexcept;
  [[nodiscard]] NativeFunctionResult
  invokeSupportsCapability(const NativeCallView &call) noexcept;
  [[nodiscard]] NativeFunctionResult
  encodeNativeResult(const EnqueueResult &result) noexcept;
  [[nodiscard]] EnqueueResult
  submitCoreMessage(CoreInboundMessage message) noexcept;
  void dispatchCallbackOnExecutor(JsInboundMessage message,
                                  std::uint64_t admittedGeneration) noexcept;
  void dispatchToConsumer(const JsInboundMessage &message) noexcept;
  void observeBridge(std::string_view markerName, const CorrelationKey &key,
                     const std::optional<std::string> &surfaceId,
                     std::optional<std::string_view> errorCode =
                         std::nullopt) noexcept;
  void unbindAllOnExecutor() noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  admittedGeneration(const JsInboundMessage &message) const noexcept;
  [[nodiscard]] bool onExecutor() const noexcept;

  JsEngineService &engineService_;
  CoreIngressPort &corePort_;
  RuntimeAbiLimits limits_;
  CapabilitySupportSnapshot capabilitySnapshot_;
  std::atomic<RuntimeAbiServiceState> state_{RuntimeAbiServiceState::New};
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  std::vector<NativeBindingToken> bindingTokens_;
  std::map<CorrelationKey, PendingRecord> correlations_;
  CallbackSlots callbackSlots_;
  std::uint64_t activeConsumerRegistration_{0};
  std::uint64_t nextConsumerRegistration_{0};
  mutable std::mutex surfaceMutex_;
  std::map<std::string, SurfaceScope, std::less<>> surfaces_;
  std::atomic<std::size_t> queuedCallbacks_{0};
};

} // namespace quickapp::js::abi
