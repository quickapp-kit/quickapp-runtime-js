#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "quickapp/js/engine/result.h"
#include "quickapp/js/engine/runtime_value.h"

namespace quickapp::js::abi {

inline constexpr std::string_view kRuntimeAbiIdentity =
    "quickapp-kit-runtime-v1";
inline constexpr std::uint64_t kRuntimeAbiSchemaVersion = 1;

enum class AbiErrorCode {
  InvalidArgument,
  UnsupportedVersion,
  SurfaceNotFound,
  OutOfMemory,
  QueueOverflow,
  JsException,
  PortClosed,
};

struct AbiRuntimeError {
  AbiErrorCode code{AbiErrorCode::InvalidArgument};
  std::string message;
  bool retryable{false};
  std::optional<std::string> surfaceId;
  std::optional<std::string> requestId;
  std::optional<std::string> transactionId;
  std::optional<std::string> mountAttemptId;

  friend bool operator==(const AbiRuntimeError &,
                         const AbiRuntimeError &) = default;
};

struct EnqueueResult {
  bool ok{false};
  std::optional<AbiRuntimeError> error;

  [[nodiscard]] static EnqueueResult accepted();
  [[nodiscard]] static EnqueueResult rejected(AbiRuntimeError error);
  friend bool operator==(const EnqueueResult &, const EnqueueResult &) =
      default;
};

[[nodiscard]] std::string_view abiErrorCodeName(AbiErrorCode code) noexcept;

enum class CoreMessageKind {
  InstantiateTemplate,
  CompleteVerifiedModuleLoad,
  CompleteVmInitialization,
  SubmitRenderTransaction,
  RegisterHandler,
  UnregisterHandler,
  NavigationPush,
  NavigationClose,
  ShowToast,
  FeatureRequest,
  DeviceGetInfo,
  TimerStart,
  TimerCancel,
  SetTitleBar,
  SetMeta,
  CompleteLifecycle,
};

enum class JsCallbackKind {
  LoadVerifiedModule,
  AppContext,
  SurfaceContext,
  VmInitializationDispatch,
  LifecycleDispatch,
  JsEventDispatch,
  InstantiateTemplateResult,
  HandlerRegistrationResult,
  RenderTransactionResult,
  NavigationPushResult,
  NavigationCloseResult,
  ShowToastResult,
  FeatureResult,
  DeviceGetInfoResult,
  TimerStartResult,
  TimerCancelResult,
  TimerFired,
  SetTitleBarResult,
  SetMetaResult,
  SurfaceStatusChanged,
};

using DynamicValues = std::map<std::string, RuntimeValue, std::less<>>;
using BindingValue = std::variant<std::string, bool, double>;
using BindingValues = std::map<std::uint64_t, BindingValue>;
using BlockKey = std::variant<std::string, double>;
using ImmutableByteStorage =
    std::shared_ptr<const std::vector<std::uint8_t>>;

struct MessageRuntimeError {
  std::string code;
  std::string message;
  bool retryable{false};
  std::optional<std::string> surfaceId;
  std::optional<std::string> requestId;
  std::optional<std::string> transactionId;
  std::optional<std::string> mountAttemptId;
};

struct LogicalNodeRef {
  std::string ownerInstanceId;
  std::uint64_t templateNodeId{0};
};

struct HandlerBinding {
  std::string ownerInstanceId;
  std::uint64_t templateHandlerId{0};
  std::string handlerId;
};

struct InstantiateBlockOperation {
  std::uint64_t templateBlockId{0};
  std::string blockInstanceId;
  LogicalNodeRef parent;
  std::uint64_t index{0};
  std::optional<BlockKey> key;
  BindingValues initialBindings;
  std::vector<HandlerBinding> handlers;
};

struct RemoveBlockOperation { std::string blockInstanceId; };
struct MoveBlockOperation {
  std::string blockInstanceId;
  LogicalNodeRef parent;
  std::uint64_t index{0};
};
struct UpdateBindingOperation {
  std::string ownerInstanceId;
  std::uint64_t templateBindingId{0};
  BindingValue value;
};
using RenderOperation =
    std::variant<UpdateBindingOperation, InstantiateBlockOperation,
                 RemoveBlockOperation, MoveBlockOperation>;

struct ModuleBundle {
  std::string path;
  std::uint64_t byteLength{0};
  std::string sha256;
  ImmutableByteStorage bytes;
};

struct BootstrapExpectation {
  std::string kind;
  std::string moduleId;
  std::optional<std::string> templateId;
};

struct Viewport {
  double width{0};
  double height{0};
  std::string unit;
};

struct DeviceInfo {
  std::string osType;
  std::string platformVersionName;
  std::uint64_t platformVersionCode{0};
  double screenDensity{0};
  double screenWidth{0};
  double screenHeight{0};
  double windowWidth{0};
  double windowHeight{0};
  std::string deviceType;
  std::optional<std::string> brand;
  std::optional<std::string> manufacturer;
  std::optional<std::string> model;
  std::optional<std::string> product;
  std::optional<std::string> osVersionName;
  std::optional<std::uint64_t> osVersionCode;
};

struct InstantiateTemplate {
  static constexpr auto kind = CoreMessageKind::InstantiateTemplate;
  std::string requestId, surfaceId, templateId, ownerInstanceId;
  BindingValues initialBindings;
  std::vector<InstantiateBlockOperation> initialBlocks;
  std::vector<HandlerBinding> initialHandlers;
};
struct CompleteVerifiedModuleLoad {
  static constexpr auto kind = CoreMessageKind::CompleteVerifiedModuleLoad;
  std::string requestId, moduleKind, moduleId, status;
  std::optional<std::string> surfaceId;
  std::optional<MessageRuntimeError> error;
};
struct CompleteVmInitialization {
  static constexpr auto kind = CoreMessageKind::CompleteVmInitialization;
  std::string requestId, scope, status;
  std::optional<std::string> surfaceId, failedPhase;
  std::optional<MessageRuntimeError> error;
};
struct SubmitRenderTransaction {
  static constexpr auto kind = CoreMessageKind::SubmitRenderTransaction;
  std::string surfaceId, transactionId;
  std::uint64_t revision{0};
  std::optional<std::string> requestId;
  std::vector<RenderOperation> operations;
};
struct RegisterHandler {
  static constexpr auto kind = CoreMessageKind::RegisterHandler;
  std::string requestId, surfaceId, ownerInstanceId, handlerId;
  std::uint64_t templateHandlerId{0};
};
struct UnregisterHandler {
  static constexpr auto kind = CoreMessageKind::UnregisterHandler;
  std::string requestId, surfaceId, handlerId;
};
struct NavigationPush {
  static constexpr auto kind = CoreMessageKind::NavigationPush;
  std::string requestId, sourceSurfaceId, uri;
  DynamicValues params;
};
struct NavigationClose {
  static constexpr auto kind = CoreMessageKind::NavigationClose;
  std::string requestId, sourceSurfaceId;
};
struct ShowToast {
  static constexpr auto kind = CoreMessageKind::ShowToast;
  std::string requestId, surfaceId, message;
  std::uint64_t durationMs{0};
};
enum class FeatureModule { Prompt, Fetch, File, OpenUrl, Webview };
enum class FeatureMethod {
  Alert,
  Confirm,
  Fetch,
  FetchCancel,
  FileRead,
  FileWrite,
  FileExists,
  FileDelete,
  OpenUrl,
  WebviewOpen,
};
struct FeatureHeader {
  std::string name;
  std::string value;
};
struct FeatureRequest {
  static constexpr auto kind = CoreMessageKind::FeatureRequest;
  std::string requestId, surfaceId;
  FeatureModule module{FeatureModule::Prompt};
  FeatureMethod method{FeatureMethod::Alert};
  std::string text;
  std::string url;
  std::string httpMethod;
  std::vector<FeatureHeader> headers;
  std::optional<std::string> body;
  std::uint64_t timeoutMs{0};
  std::string responseType;
  std::string targetRequestId;
  std::string path;
  std::optional<std::string> data;
};
struct DeviceGetInfo {
  static constexpr auto kind = CoreMessageKind::DeviceGetInfo;
  std::string requestId, surfaceId;
};
struct TimerStart {
  static constexpr auto kind = CoreMessageKind::TimerStart;
  std::string requestId, surfaceId;
  std::uint64_t delayMs{0};
  std::uint64_t periodMs{0};
};
struct TimerCancel {
  static constexpr auto kind = CoreMessageKind::TimerCancel;
  std::string requestId, surfaceId, timerId;
};
struct SetTitleBar {
  static constexpr auto kind = CoreMessageKind::SetTitleBar;
  std::string requestId, surfaceId, text;
};
struct SetMeta {
  static constexpr auto kind = CoreMessageKind::SetMeta;
  std::string requestId, surfaceId;
  std::optional<std::string> title, description;
};
struct CompleteLifecycle {
  static constexpr auto kind = CoreMessageKind::CompleteLifecycle;
  std::string requestId, scope, hook, status;
  std::uint64_t sequence{0};
  std::optional<std::string> surfaceId;
  std::optional<MessageRuntimeError> error;
};

using CoreInboundMessage =
    std::variant<InstantiateTemplate, CompleteVerifiedModuleLoad,
                 CompleteVmInitialization, SubmitRenderTransaction,
                 RegisterHandler, UnregisterHandler, NavigationPush,
                 NavigationClose, ShowToast, FeatureRequest, DeviceGetInfo, TimerStart,
                 TimerCancel, SetTitleBar,
                 SetMeta, CompleteLifecycle>;

struct LoadVerifiedModule {
  static constexpr auto kind = JsCallbackKind::LoadVerifiedModule;
  std::string requestId, packageId, moduleKind, moduleId, cacheScope;
  std::optional<std::string> surfaceId;
  ModuleBundle bundle;
  std::vector<std::string> dependencies;
  std::optional<BootstrapExpectation> expectedBootstrap;
  std::optional<std::vector<std::uint64_t>> expectedBindingIds;
  std::optional<std::vector<std::uint64_t>> expectedHandlerIds;
};
struct AppContext {
  static constexpr auto kind = JsCallbackKind::AppContext;
  std::string packageId, versionName, runtimeVersion;
  std::uint64_t versionCode{0};
  std::vector<std::string> declaredCapabilities;
};
struct SurfaceContext {
  static constexpr auto kind = JsCallbackKind::SurfaceContext;
  std::string surfaceId, packageId, route, templateId;
  DynamicValues params;
  std::vector<std::string> hostCapabilities;
  Viewport viewport;
};
struct VmInitializationDispatch {
  static constexpr auto kind = JsCallbackKind::VmInitializationDispatch;
  std::string requestId, scope;
  std::optional<std::string> surfaceId;
};
struct LifecycleDispatch {
  static constexpr auto kind = JsCallbackKind::LifecycleDispatch;
  std::string requestId, scope, hook;
  std::uint64_t sequence{0};
  std::optional<std::string> surfaceId;
};
struct JsEventDispatch {
  static constexpr auto kind = JsCallbackKind::JsEventDispatch;
  std::string requestId, surfaceId, handlerId, eventType, phase;
  LogicalNodeRef target, currentTarget;
  double timestamp{0};
  DynamicValues payload;
};
struct InstantiateTemplateResult {
  static constexpr auto kind = JsCallbackKind::InstantiateTemplateResult;
  std::string requestId, status, surfaceId;
  std::optional<std::uint64_t> committedRevision;
  std::optional<MessageRuntimeError> error;
};
struct HandlerRegistrationResult {
  static constexpr auto kind = JsCallbackKind::HandlerRegistrationResult;
  std::string requestId, operation, status, surfaceId, handlerId;
  std::optional<MessageRuntimeError> error;
};
struct RenderTransactionResult {
  static constexpr auto kind = JsCallbackKind::RenderTransactionResult;
  std::string surfaceId, transactionId, status;
  std::uint64_t submittedRevision{0}, committedRevision{0};
  std::optional<MessageRuntimeError> error;
};
struct NavigationPushResult {
  static constexpr auto kind = JsCallbackKind::NavigationPushResult;
  std::string requestId, sourceSurfaceId, status;
  std::optional<std::string> targetSurfaceId;
  std::optional<MessageRuntimeError> error;
};
struct NavigationCloseResult {
  static constexpr auto kind = JsCallbackKind::NavigationCloseResult;
  std::string requestId, sourceSurfaceId, status;
  std::optional<std::string> revealedSurfaceId;
  std::optional<MessageRuntimeError> error;
};
struct ShowToastResult {
  static constexpr auto kind = JsCallbackKind::ShowToastResult;
  std::string requestId, surfaceId, status;
  std::optional<MessageRuntimeError> error;
};
struct FeatureResult {
  static constexpr auto kind = JsCallbackKind::FeatureResult;
  std::string requestId, surfaceId, status;
  std::optional<bool> confirmed;
  std::optional<std::uint64_t> httpStatus;
  std::optional<std::string> responseBody;
  std::optional<bool> responseIsJson;
  std::optional<std::string> fileData;
  std::optional<bool> fileExists;
  std::optional<MessageRuntimeError> error;
};
struct DeviceGetInfoResult {
  static constexpr auto kind = JsCallbackKind::DeviceGetInfoResult;
  std::string requestId, surfaceId, status;
  std::optional<DeviceInfo> info;
  std::optional<MessageRuntimeError> error;
};
struct TimerStartResult {
  static constexpr auto kind = JsCallbackKind::TimerStartResult;
  std::string requestId, surfaceId, status;
  std::optional<std::string> timerId;
  std::optional<MessageRuntimeError> error;
};
struct TimerCancelResult {
  static constexpr auto kind = JsCallbackKind::TimerCancelResult;
  std::string requestId, surfaceId, status, timerId;
  std::optional<MessageRuntimeError> error;
};
struct TimerFired {
  static constexpr auto kind = JsCallbackKind::TimerFired;
  std::string surfaceId, timerId;
  std::uint64_t sequence{0};
  std::uint64_t missedPeriods{0};
};
struct SetTitleBarResult {
  static constexpr auto kind = JsCallbackKind::SetTitleBarResult;
  std::string requestId, surfaceId, status;
  std::optional<MessageRuntimeError> error;
};
struct SetMetaResult {
  static constexpr auto kind = JsCallbackKind::SetMetaResult;
  std::string requestId, surfaceId, status;
  std::optional<MessageRuntimeError> error;
};
struct SurfaceStatusChanged {
  static constexpr auto kind = JsCallbackKind::SurfaceStatusChanged;
  std::string surfaceId, lifecycleState, healthState;
  std::uint64_t committedRevision{0};
};

using JsInboundMessage =
    std::variant<LoadVerifiedModule, AppContext, SurfaceContext,
                 VmInitializationDispatch, LifecycleDispatch, JsEventDispatch,
                 InstantiateTemplateResult, HandlerRegistrationResult,
                 RenderTransactionResult, NavigationPushResult,
                 NavigationCloseResult, ShowToastResult, FeatureResult, DeviceGetInfoResult,
                 TimerStartResult, TimerCancelResult, TimerFired,
                 SetTitleBarResult, SetMetaResult, SurfaceStatusChanged>;

enum class CorrelationKeyKind { Request, Transaction };

struct CorrelationKey {
  CorrelationKeyKind kind{CorrelationKeyKind::Request};
  std::string value;

  friend bool operator==(const CorrelationKey &, const CorrelationKey &) =
      default;
  friend auto operator<=>(const CorrelationKey &,
                          const CorrelationKey &) = default;
};

enum class CorrelationOwnerKind { AppRuntime, Surface };

struct CorrelationOwner {
  CorrelationOwnerKind kind{CorrelationOwnerKind::AppRuntime};
  std::string surfaceId;

  friend bool operator==(const CorrelationOwner &,
                         const CorrelationOwner &) = default;
};

struct PendingRecord {
  CorrelationKey key;
  JsCallbackKind expectedResultKind{JsCallbackKind::AppContext};
  CorrelationOwner owner;
  std::uint64_t ownerGeneration{0};

  friend bool operator==(const PendingRecord &, const PendingRecord &) =
      default;
};

class CoreIngressPort {
public:
  virtual ~CoreIngressPort() = default;
  [[nodiscard]] virtual EnqueueResult
  post(CoreInboundMessage message) noexcept = 0;
};

struct CapabilitySupportSnapshot {
  std::set<std::pair<std::string, std::string>> methods;

  [[nodiscard]] bool supports(std::string_view module,
                              std::string_view method) const;
};

struct CallbackSlots {
  std::function<void(const LoadVerifiedModule &)> loadVerifiedModule;
  std::function<void(const AppContext &)> appContext;
  std::function<void(const SurfaceContext &)> surfaceContext;
  std::function<void(const VmInitializationDispatch &)>
      vmInitializationDispatch;
  std::function<void(const LifecycleDispatch &)> lifecycleDispatch;
  std::function<void(const JsEventDispatch &)> jsEventDispatch;
  std::function<void(const InstantiateTemplateResult &)>
      instantiateTemplateResult;
  std::function<void(const HandlerRegistrationResult &)>
      handlerRegistrationResult;
  std::function<void(const RenderTransactionResult &)>
      renderTransactionResult;
  std::function<void(const NavigationPushResult &)> navigationPushResult;
  std::function<void(const NavigationCloseResult &)> navigationCloseResult;
  std::function<void(const ShowToastResult &)> showToastResult;
  std::function<void(const FeatureResult &)> featureResult;
  std::function<void(const DeviceGetInfoResult &)> deviceGetInfoResult;
  std::function<void(const TimerStartResult &)> timerStartResult;
  std::function<void(const TimerCancelResult &)> timerCancelResult;
  std::function<void(const TimerFired &)> timerFired;
  std::function<void(const SetTitleBarResult &)> setTitleBarResult;
  std::function<void(const SetMetaResult &)> setMetaResult;
  std::function<void(const SurfaceStatusChanged &)> surfaceStatusChanged;
};

class ConsumerRegistrationToken {
public:
  ConsumerRegistrationToken() = default;
  ConsumerRegistrationToken(ConsumerRegistrationToken &&other) noexcept;
  ConsumerRegistrationToken &
  operator=(ConsumerRegistrationToken &&other) noexcept;
  ConsumerRegistrationToken(const ConsumerRegistrationToken &) = delete;
  ConsumerRegistrationToken &
  operator=(const ConsumerRegistrationToken &) = delete;

  [[nodiscard]] bool valid() const noexcept { return id_ != 0; }

private:
  explicit ConsumerRegistrationToken(std::uint64_t id) : id_(id) {}
  std::uint64_t id_{0};
  friend class RuntimeAbiService;
};

struct RuntimeAbiLimits {
  std::size_t maxBridgeCorrelations{256};
  ValueLimits valueLimits{64, 10000};
};

struct RuntimeAbiResourceSnapshot {
  std::size_t liveNativeEntries{0};
  std::size_t liveBridgeCorrelations{0};
  std::size_t liveConsumerRegistrations{0};
  std::size_t openSurfaceScopes{0};
  std::size_t queuedAbiCallbacks{0};
};

[[nodiscard]] CoreMessageKind messageKind(const CoreInboundMessage &message);
[[nodiscard]] JsCallbackKind callbackKind(const JsInboundMessage &message);
[[nodiscard]] std::string_view coreMessageKindName(CoreMessageKind kind);
[[nodiscard]] std::string_view callbackKindName(JsCallbackKind kind);

} // namespace quickapp::js::abi
