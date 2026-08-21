#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "quickapp/js/abi/runtime_abi_service.h"
#include "quickapp/js/engine/fake_engine_provider.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"

namespace {

using namespace quickapp::js;
using namespace quickapp::js::abi;
using quickapp::js::testing::FakeEngineProvider;

class TestFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                          \
                        std::to_string(__LINE__) +                             \
                        ": CHECK(" #condition ") failed");                    \
    }                                                                          \
  } while (false)

class TestClock final : public MonotonicClock {
public:
  std::uint64_t nowNs() const noexcept override {
    return now_.fetch_add(100, std::memory_order_relaxed);
  }

private:
  mutable std::atomic<std::uint64_t> now_{1000};
};

class RecordingSink final : public TraceSink {
public:
  void emit(const TraceEvent &event) noexcept override {
    std::lock_guard lock(mutex_);
    markers_.emplace_back(event.markerName);
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex_);
    return markers_.size();
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> markers_;
};

class FakeCorePort final : public CoreIngressPort {
public:
  EnqueueResult post(CoreInboundMessage message) noexcept override {
    std::lock_guard lock(mutex_);
    ++postCount_;
    if (nextResult_) {
      auto result = std::move(*nextResult_);
      nextResult_.reset();
      if (!result.ok) {
        return result;
      }
    }
    messages_.push_back(std::move(message));
    return EnqueueResult::accepted();
  }

  void rejectNext(AbiErrorCode code) {
    std::lock_guard lock(mutex_);
    nextResult_ = EnqueueResult::rejected(
        {code, "injected Core rejection", true, std::nullopt, std::nullopt,
         std::nullopt, std::nullopt});
  }

  [[nodiscard]] std::size_t messageCount() const {
    std::lock_guard lock(mutex_);
    return messages_.size();
  }

  [[nodiscard]] std::size_t postCount() const {
    std::lock_guard lock(mutex_);
    return postCount_;
  }

  [[nodiscard]] std::vector<CoreMessageKind> kinds() const {
    std::lock_guard lock(mutex_);
    std::vector<CoreMessageKind> result;
    result.reserve(messages_.size());
    for (const auto &message : messages_) {
      result.push_back(messageKind(message));
    }
    return result;
  }

  [[nodiscard]] CoreInboundMessage messageAt(std::size_t index) const {
    std::lock_guard lock(mutex_);
    return messages_.at(index);
  }

private:
  mutable std::mutex mutex_;
  std::optional<EnqueueResult> nextResult_;
  std::vector<CoreInboundMessage> messages_;
  std::size_t postCount_{0};
};

// Test-only model of the later Framework bootstrap-owned allocator.
class TestJsRequestIdAllocator final {
public:
  [[nodiscard]] std::string next() {
    return "req:j-" + std::to_string(next_++);
  }

private:
  std::uint64_t next_{1};
};

RuntimeValue::Object object(
    std::initializer_list<std::pair<const std::string, RuntimeValue>> values) {
  return RuntimeValue::Object(values);
}

RuntimeValue errorValue(std::string code = "PLATFORM_REJECTED") {
  return RuntimeValue(object({{"code", RuntimeValue(std::move(code))},
                              {"message", RuntimeValue("failed")},
                              {"retryable", RuntimeValue(false)}}));
}

RuntimeValue outbound(CoreMessageKind kind, std::string id = {}) {
  switch (kind) {
  case CoreMessageKind::InstantiateTemplate:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("instantiateTemplate")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-1" : id)},
        {"surfaceId", RuntimeValue("srf:1")},
        {"templateId", RuntimeValue("page:index")},
        {"ownerInstanceId", RuntimeValue("cmp:1")},
        {"initialBindings", RuntimeValue(RuntimeValue::Object{})},
        {"initialBlocks", RuntimeValue(RuntimeValue::Array{})},
        {"initialHandlers", RuntimeValue(RuntimeValue::Array{})},
    }));
  case CoreMessageKind::CompleteVerifiedModuleLoad:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("loadVerifiedModuleResult")},
        {"requestId", RuntimeValue(id.empty() ? "req:1" : id)},
        {"moduleKind", RuntimeValue("app")},
        {"moduleId", RuntimeValue("app")},
        {"status", RuntimeValue("loaded")},
    }));
  case CoreMessageKind::CompleteVmInitialization:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("vmInitializationResult")},
        {"requestId", RuntimeValue(id.empty() ? "req:2" : id)},
        {"scope", RuntimeValue("app")},
        {"status", RuntimeValue("completed")},
    }));
  case CoreMessageKind::SubmitRenderTransaction:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"surfaceId", RuntimeValue("srf:1")},
        {"revision", RuntimeValue(1.0)},
        {"transactionId", RuntimeValue(id.empty() ? "txn:1" : id)},
        {"operations", RuntimeValue(RuntimeValue::Array{})},
    }));
  case CoreMessageKind::RegisterHandler:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("registerHandler")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-2" : id)},
        {"surfaceId", RuntimeValue("srf:1")},
        {"ownerInstanceId", RuntimeValue("cmp:1")},
        {"templateHandlerId", RuntimeValue(1.0)},
        {"handlerId", RuntimeValue("hdl:1")},
    }));
  case CoreMessageKind::UnregisterHandler:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("unregisterHandler")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-3" : id)},
        {"surfaceId", RuntimeValue("srf:1")},
        {"handlerId", RuntimeValue("hdl:1")},
    }));
  case CoreMessageKind::NavigationPush:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("navigationPush")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-4" : id)},
        {"sourceSurfaceId", RuntimeValue("srf:1")},
        {"uri", RuntimeValue("/next")},
        {"params", RuntimeValue(RuntimeValue::Object{})},
    }));
  case CoreMessageKind::NavigationClose:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("navigationClose")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-5" : id)},
        {"sourceSurfaceId", RuntimeValue("srf:1")},
    }));
  case CoreMessageKind::ShowToast:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("showToast")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-6" : id)},
        {"surfaceId", RuntimeValue("srf:1")},
        {"message", RuntimeValue("hello")},
        {"durationMs", RuntimeValue(1000.0)},
    }));
  case CoreMessageKind::DeviceGetInfo:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("deviceGetInfo")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-7" : id)},
        {"surfaceId", RuntimeValue("srf:1")},
    }));
  case CoreMessageKind::SetTitleBar:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("setTitleBar")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-8" : id)},
        {"surfaceId", RuntimeValue("srf:1")},
        {"text", RuntimeValue("title")},
    }));
  case CoreMessageKind::SetMeta:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("setMeta")},
        {"requestId", RuntimeValue(id.empty() ? "req:j-9" : id)},
        {"surfaceId", RuntimeValue("srf:1")},
        {"title", RuntimeValue("title")},
    }));
  case CoreMessageKind::CompleteLifecycle:
    return RuntimeValue(object({
        {"schemaVersion", RuntimeValue(1.0)},
        {"kind", RuntimeValue("lifecycleResult")},
        {"requestId", RuntimeValue(id.empty() ? "req:3" : id)},
        {"scope", RuntimeValue("app")},
        {"hook", RuntimeValue("onShow")},
        {"sequence", RuntimeValue(1.0)},
        {"status", RuntimeValue("completed")},
    }));
  }
  return RuntimeValue(nullptr);
}

JsInboundMessage resultFor(JsCallbackKind kind, std::string id) {
  switch (kind) {
  case JsCallbackKind::InstantiateTemplateResult:
    return InstantiateTemplateResult{std::move(id), "presented", "srf:1", 0,
                                     std::nullopt};
  case JsCallbackKind::HandlerRegistrationResult:
    return HandlerRegistrationResult{std::move(id), "register", "registered",
                                     "srf:1", "hdl:1", std::nullopt};
  case JsCallbackKind::RenderTransactionResult:
    return RenderTransactionResult{"srf:1", std::move(id), "presented", 1, 1,
                                   std::nullopt};
  case JsCallbackKind::NavigationPushResult:
    return NavigationPushResult{std::move(id), "srf:1", "presented", "srf:2",
                                std::nullopt};
  case JsCallbackKind::NavigationCloseResult:
    return NavigationCloseResult{std::move(id), "srf:1", "closed", "srf:2",
                                 std::nullopt};
  case JsCallbackKind::ShowToastResult:
    return ShowToastResult{std::move(id), "srf:1", "completed", std::nullopt};
  case JsCallbackKind::DeviceGetInfoResult:
    return DeviceGetInfoResult{std::move(id), "srf:1", "completed",
                               DeviceInfo{}, std::nullopt};
  case JsCallbackKind::SetTitleBarResult:
    return SetTitleBarResult{std::move(id), "srf:1", "completed", std::nullopt};
  case JsCallbackKind::SetMetaResult:
    return SetMetaResult{std::move(id), "srf:1", "completed", std::nullopt};
  default:
    throw TestFailure("unsupported result fixture");
  }
}

ImmutableByteStorage immutableBytes(std::string_view value) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(value.size());
  for (const auto character : value) {
    bytes.push_back(static_cast<std::uint8_t>(character));
  }
  return std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
}

LoadVerifiedModule appModuleCallback(ImmutableByteStorage bytes) {
  return LoadVerifiedModule{
      "req:4", "pkg", "app", "app", "appRuntime", std::nullopt,
      ModuleBundle{"app.js", 1, std::string(64, '0'), std::move(bytes)}, {},
      BootstrapExpectation{"app", "app", std::nullopt}, std::nullopt,
      std::nullopt};
}

LoadVerifiedModule pageModuleCallback(ImmutableByteStorage bytes) {
  return LoadVerifiedModule{
      "req:4", "pkg", "page", "page:index", "surface", "srf:1",
      ModuleBundle{"pages/index.js", 1, std::string(64, '0'),
                   std::move(bytes)},
      {}, BootstrapExpectation{"page", "page:index", "page:index"},
      std::vector<std::uint64_t>{1}, std::vector<std::uint64_t>{1}};
}

std::vector<JsInboundMessage> dispatchFixtures() {
  return {
      appModuleCallback(immutableBytes("a")),
      AppContext{"pkg", "1.0", "v1", 1, {}},
      SurfaceContext{"srf:1", "pkg", "/index", "page:index", {}, {},
                     Viewport{320, 480, "logical-px"}},
      VmInitializationDispatch{"req:5", "page", "srf:1"},
      LifecycleDispatch{"req:6", "page", "onShow", 1, "srf:1"},
      JsEventDispatch{"req:p-1", "srf:1", "hdl:1", "click", "target",
                      LogicalNodeRef{"cmp:1", 1},
                      LogicalNodeRef{"cmp:1", 1}, 1.0, {}},
      SurfaceStatusChanged{"srf:1", "visible", "normal", 1},
  };
}

JsEngineConfig engineConfig(const JsEngineDescriptor &descriptor,
                            std::uint32_t maxPendingTasks = 16) {
  JsEngineConfig config;
  config.expectedEngine = descriptor;
  config.limits.maxHeapBytes = 32ULL * 1024ULL * 1024ULL;
  config.limits.maxStackBytes = 512ULL * 1024ULL;
  config.limits.maxPendingTasks = maxPendingTasks;
  config.limits.maxMicrotasksPerTurn = 8;
  config.limits.maxRuntimeValueDepth = 32;
  config.limits.maxRuntimeValueNodes = 2048;
  return config;
}

struct Harness {
  TestClock clock;
  std::unique_ptr<TraceSink> sink;
  RecordingSink *recordingSink{nullptr};
  FakeCorePort core;
  std::unique_ptr<JsEngineService> engine;
  std::shared_ptr<RuntimeAbiService> abi;
  ConsumerRegistrationToken consumerToken;

  explicit Harness(std::unique_ptr<JsEngineProvider> provider,
                   std::size_t maxCorrelations = 10,
                   std::uint32_t maxPendingTasks = 16,
                   bool useNoopSink = false) {
    if (useNoopSink) {
      sink = std::make_unique<NoopTraceSink>();
    } else {
      auto recording = std::make_unique<RecordingSink>();
      recordingSink = recording.get();
      sink = std::move(recording);
    }
    auto admitted = TraceSinkRegistration::admit(
        *sink, {.nonblocking = true, .noReentry = true});
    CHECK(admitted.ok());
    const auto descriptor = provider->describe();
    engine = std::make_unique<JsEngineService>(
        "app:1", std::move(provider),
        engineConfig(descriptor, maxPendingTasks), clock,
        std::move(admitted).value(),
        ObservationConfig{true, "run:js-s02", "steady", 0});
    std::promise<ServiceResult> started;
    CHECK(engine->start(
        [&](ServiceResult result) { started.set_value(std::move(result)); }));
    CHECK(started.get_future().get().ok());
    abi = std::make_shared<RuntimeAbiService>(
        *engine, core,
        RuntimeAbiLimits{maxCorrelations, ValueLimits{32, 2048}},
        CapabilitySupportSnapshot{{{"system.prompt", "showToast"}}});
  }

  ~Harness() {
    if (engine && engine->state() == EngineServiceState::Running) {
      stop();
    }
  }

  template <typename F> auto onExecutor(F function) {
    using R = decltype(function(std::declval<JsEnginePort &>(),
                                std::declval<const JsContextRef &>()));
    auto promise = std::make_shared<std::promise<R>>();
    auto future = promise->get_future();
    const auto posted = engine->post(
        [promise, function = std::move(function)](
            JsEnginePort &port, const JsContextRef &context) mutable {
          try {
            promise->set_value(function(port, context));
          } catch (...) {
            promise->set_exception(std::current_exception());
          }
        });
    CHECK(posted.status == PostStatus::Accepted);
    return future.get();
  }

  void startAbi(std::string identity = std::string(kRuntimeAbiIdentity)) {
    auto result = onExecutor([&](JsEnginePort &port,
                                 const JsContextRef &context) {
      return abi->startOnExecutor(port, context, identity);
    });
    CHECK(result.ok());
  }

  void openSurface(std::string id = "srf:1") {
    auto result = onExecutor([&](JsEnginePort &, const JsContextRef &) {
      return abi->openSurfaceOnExecutor(std::move(id));
    });
    CHECK(result.ok());
  }

  RuntimeValue callBinding(std::string name,
                           std::vector<RuntimeValue> arguments) {
    return onExecutor([name = std::move(name), arguments = std::move(arguments)](
                          JsEnginePort &port, const JsContextRef &context) mutable {
      auto globalResult = port.globalObject(context);
      CHECK(globalResult.ok());
      auto global = std::move(globalResult).value();
      auto functionResult = port.getProperty(context, global, name);
      CHECK(functionResult.ok());
      auto function = std::move(functionResult).value();
      std::vector<JsValueRef> values;
      values.reserve(arguments.size());
      for (const auto &argument : arguments) {
        auto value = port.fromRuntimeValue(context, argument);
        CHECK(value.ok());
        values.push_back(std::move(value).value());
      }
      auto called = port.call(context, function, global, values);
      CHECK(called.ok());
      auto resultValue = std::move(called).value();
      auto converted =
          port.toRuntimeValue(context, resultValue, ValueLimits{32, 2048});
      CHECK(converted.ok());
      RuntimeValue result = converted.value();
      resultValue.reset();
      for (auto &value : values) {
        value.reset();
      }
      function.reset();
      global.reset();
      return result;
    });
  }

  void barrier() {
    onExecutor([](JsEnginePort &, const JsContextRef &) { return true; });
  }

  RuntimeValue evaluate(std::string source) {
    return onExecutor([source = std::move(source)](
                          JsEnginePort &port, const JsContextRef &context) {
      auto evaluated = port.evaluate(
          context, SourceUnit{"js-s02-strict", "test://js-s02-strict.js",
                              source, SourceMode::Script});
      CHECK(evaluated.ok());
      auto value = std::move(evaluated).value();
      auto converted =
          port.toRuntimeValue(context, value, ValueLimits{32, 2048});
      CHECK(converted.ok());
      auto result = converted.value();
      value.reset();
      return result;
    });
  }

  void stop() {
    std::promise<void> stopped;
    CHECK(engine->stop([runtime = abi] { runtime->stopOnExecutor(); },
                       [&] { stopped.set_value(); }));
    stopped.get_future().get();
  }
};

bool enqueueOk(const RuntimeValue &value) {
  const auto *objectValue =
      std::get_if<RuntimeValue::Object>(&value.storage());
  if (!objectValue) {
    return false;
  }
  const auto found = objectValue->find("ok");
  return found != objectValue->end() &&
         std::holds_alternative<bool>(found->second.storage()) &&
         std::get<bool>(found->second.storage());
}

std::string bindingName(CoreMessageKind kind) {
  static const std::map<CoreMessageKind, std::string> names{
      {CoreMessageKind::InstantiateTemplate,
       "$quickapp_runtime_v1_instantiateTemplate$"},
      {CoreMessageKind::CompleteVerifiedModuleLoad,
       "$quickapp_runtime_v1_completeVerifiedModuleLoad$"},
      {CoreMessageKind::CompleteVmInitialization,
       "$quickapp_runtime_v1_completeVmInitialization$"},
      {CoreMessageKind::SubmitRenderTransaction,
       "$quickapp_runtime_v1_submitRenderTransaction$"},
      {CoreMessageKind::RegisterHandler,
       "$quickapp_runtime_v1_registerHandler$"},
      {CoreMessageKind::UnregisterHandler,
       "$quickapp_runtime_v1_unregisterHandler$"},
      {CoreMessageKind::NavigationPush, "$quickapp_runtime_v1_pushRoute$"},
      {CoreMessageKind::NavigationClose, "$quickapp_runtime_v1_closeRoute$"},
      {CoreMessageKind::ShowToast, "$quickapp_runtime_v1_showToast$"},
      {CoreMessageKind::DeviceGetInfo, "$quickapp_runtime_v1_getDeviceInfo$"},
      {CoreMessageKind::SetTitleBar, "$quickapp_runtime_v1_setTitleBar$"},
      {CoreMessageKind::SetMeta, "$quickapp_runtime_v1_setMeta$"},
      {CoreMessageKind::CompleteLifecycle,
       "$quickapp_runtime_v1_completeLifecycle$"},
  };
  return names.at(kind);
}

void verifyAllocatorFixture() {
  TestJsRequestIdAllocator appRuntimeAllocator;
  const auto moduleAFirst = appRuntimeAllocator.next();
  const auto moduleBFirst = appRuntimeAllocator.next();
  const auto moduleASecond = appRuntimeAllocator.next();
  CHECK(moduleAFirst == "req:j-1");
  CHECK(moduleBFirst == "req:j-2");
  CHECK(moduleASecond == "req:j-3");
}

void runAbiContractSuite(std::unique_ptr<JsEngineProvider> provider) {
  Harness harness(std::move(provider));
  harness.startAbi();
  CHECK(harness.abi->resources().liveNativeEntries == 14);
  harness.openSurface();

  std::map<JsCallbackKind, int> delivered;
  std::vector<std::string> appContextOrder;
  bool callbackOnExecutor = false;
  CallbackSlots slots;
  slots.loadVerifiedModule = [&](const LoadVerifiedModule &message) {
    CHECK(message.requestId == "req:4");
    CHECK(message.moduleKind == "app");
    ++delivered[JsCallbackKind::LoadVerifiedModule];
  };
  slots.appContext = [&](const AppContext &message) {
    ++delivered[JsCallbackKind::AppContext];
    callbackOnExecutor = harness.engine->executor().isOnExecutor();
    CHECK(message.versionCode == 1);
    appContextOrder.push_back(message.packageId);
  };
  slots.surfaceContext = [&](const SurfaceContext &message) {
    CHECK(message.viewport.unit == "logical-px");
    ++delivered[JsCallbackKind::SurfaceContext];
  };
  slots.vmInitializationDispatch = [&](const VmInitializationDispatch &message) {
    CHECK(message.surfaceId == "srf:1");
    ++delivered[JsCallbackKind::VmInitializationDispatch];
  };
  slots.lifecycleDispatch = [&](const LifecycleDispatch &message) {
    CHECK(message.sequence == 1);
    ++delivered[JsCallbackKind::LifecycleDispatch];
  };
  slots.jsEventDispatch = [&](const JsEventDispatch &message) {
    CHECK(message.target.templateNodeId == 1);
    ++delivered[JsCallbackKind::JsEventDispatch];
  };
  slots.instantiateTemplateResult = [&](const InstantiateTemplateResult &message) {
    CHECK(message.committedRevision == 0);
    ++delivered[JsCallbackKind::InstantiateTemplateResult];
  };
  slots.handlerRegistrationResult = [&](const HandlerRegistrationResult &message) {
    CHECK(message.handlerId == "hdl:1");
    ++delivered[JsCallbackKind::HandlerRegistrationResult];
  };
  slots.renderTransactionResult = [&](const RenderTransactionResult &message) {
    CHECK(message.submittedRevision == 1);
    ++delivered[JsCallbackKind::RenderTransactionResult];
  };
  slots.navigationPushResult = [&](const NavigationPushResult &message) {
    CHECK(message.targetSurfaceId == "srf:2");
    ++delivered[JsCallbackKind::NavigationPushResult];
  };
  slots.navigationCloseResult = [&](const NavigationCloseResult &message) {
    CHECK(message.revealedSurfaceId == "srf:2");
    ++delivered[JsCallbackKind::NavigationCloseResult];
  };
  slots.showToastResult = [&](const ShowToastResult &message) {
    CHECK(message.status == "completed");
    ++delivered[JsCallbackKind::ShowToastResult];
  };
  slots.deviceGetInfoResult = [&](const DeviceGetInfoResult &message) {
    CHECK(message.info.has_value());
    ++delivered[JsCallbackKind::DeviceGetInfoResult];
  };
  slots.setTitleBarResult = [&](const SetTitleBarResult &message) {
    CHECK(message.surfaceId == "srf:1");
    ++delivered[JsCallbackKind::SetTitleBarResult];
  };
  slots.setMetaResult = [&](const SetMetaResult &message) {
    CHECK(message.requestId == "req:j-9");
    ++delivered[JsCallbackKind::SetMetaResult];
  };
  slots.surfaceStatusChanged = [&](const SurfaceStatusChanged &message) {
    CHECK(message.lifecycleState == "visible");
    ++delivered[JsCallbackKind::SurfaceStatusChanged];
  };
  auto registration = harness.onExecutor(
      [&](JsEnginePort &, const JsContextRef &) {
        return harness.abi->registerConsumersOnExecutor(std::move(slots));
      });
  CHECK(registration.ok());
  harness.consumerToken = std::move(registration).value();

  auto supports = harness.callBinding(
      "$quickapp_runtime_v1_supportsCapability$",
      {RuntimeValue("system.prompt"), RuntimeValue("showToast")});
  CHECK(std::holds_alternative<bool>(supports.storage()));
  CHECK(std::get<bool>(supports.storage()));
  CHECK(harness.core.messageCount() == 0);

  const std::vector<CoreMessageKind> allKinds{
      CoreMessageKind::InstantiateTemplate,
      CoreMessageKind::CompleteVerifiedModuleLoad,
      CoreMessageKind::CompleteVmInitialization,
      CoreMessageKind::SubmitRenderTransaction,
      CoreMessageKind::RegisterHandler,
      CoreMessageKind::UnregisterHandler,
      CoreMessageKind::NavigationPush,
      CoreMessageKind::NavigationClose,
      CoreMessageKind::ShowToast,
      CoreMessageKind::DeviceGetInfo,
      CoreMessageKind::SetTitleBar,
      CoreMessageKind::SetMeta,
      CoreMessageKind::CompleteLifecycle,
  };
  for (const auto kind : allKinds) {
    CHECK(enqueueOk(
        harness.callBinding(bindingName(kind), {outbound(kind)})));
  }
  CHECK(harness.core.kinds() == allKinds);
  CHECK(std::get<InstantiateTemplate>(harness.core.messageAt(0)).templateId ==
        "page:index");
  CHECK(std::get<CompleteVerifiedModuleLoad>(harness.core.messageAt(1))
            .moduleId == "app");
  CHECK(std::get<CompleteVmInitialization>(harness.core.messageAt(2)).scope ==
        "app");
  CHECK(std::get<SubmitRenderTransaction>(harness.core.messageAt(3)).revision ==
        1);
  CHECK(std::get<RegisterHandler>(harness.core.messageAt(4))
            .templateHandlerId == 1);
  CHECK(std::get<UnregisterHandler>(harness.core.messageAt(5)).handlerId ==
        "hdl:1");
  CHECK(std::get<NavigationPush>(harness.core.messageAt(6)).uri == "/next");
  CHECK(std::get<NavigationClose>(harness.core.messageAt(7)).sourceSurfaceId ==
        "srf:1");
  CHECK(std::get<ShowToast>(harness.core.messageAt(8)).durationMs == 1000);
  CHECK(std::get<DeviceGetInfo>(harness.core.messageAt(9)).requestId ==
        "req:j-7");
  CHECK(std::get<SetTitleBar>(harness.core.messageAt(10)).text == "title");
  CHECK(std::get<SetMeta>(harness.core.messageAt(11)).title == "title");
  CHECK(std::get<CompleteLifecycle>(harness.core.messageAt(12)).sequence == 1);
  CHECK(harness.abi->resources().liveBridgeCorrelations == 10);

  auto invalidVersion = outbound(CoreMessageKind::ShowToast, "req:j-20");
  auto &invalidObject = std::get<RuntimeValue::Object>(invalidVersion.storage());
  invalidObject["schemaVersion"] = RuntimeValue(2.0);
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast), {invalidVersion})));
  auto unknownField = outbound(CoreMessageKind::ShowToast, "req:j-21");
  std::get<RuntimeValue::Object>(unknownField.storage())
      .emplace("unknown", RuntimeValue(true));
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast), {unknownField})));
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-01")})));

  harness.core.rejectNext(AbiErrorCode::QueueOverflow);
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-22")})));
  harness.core.rejectNext(AbiErrorCode::OutOfMemory);
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-23")})));
  harness.core.rejectNext(AbiErrorCode::PortClosed);
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-24")})));
  CHECK(harness.abi->resources().liveBridgeCorrelations == 10);
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-25")})));

  CHECK(harness.abi->postCallback(
            resultFor(JsCallbackKind::ShowToastResult, "req:j-7"))
            .ok);
  harness.barrier();
  CHECK(delivered[JsCallbackKind::ShowToastResult] == 0);
  CHECK(harness.abi->resources().liveBridgeCorrelations == 10);

  std::vector<std::pair<JsCallbackKind, std::string>> results{
      {JsCallbackKind::SetMetaResult, "req:j-9"},
      {JsCallbackKind::SetTitleBarResult, "req:j-8"},
      {JsCallbackKind::DeviceGetInfoResult, "req:j-7"},
      {JsCallbackKind::ShowToastResult, "req:j-6"},
      {JsCallbackKind::NavigationCloseResult, "req:j-5"},
      {JsCallbackKind::NavigationPushResult, "req:j-4"},
      {JsCallbackKind::HandlerRegistrationResult, "req:j-3"},
      {JsCallbackKind::HandlerRegistrationResult, "req:j-2"},
      {JsCallbackKind::RenderTransactionResult, "txn:1"},
      {JsCallbackKind::InstantiateTemplateResult, "req:j-1"},
  };
  for (auto &[kind, id] : results) {
    CHECK(harness.abi->postCallback(resultFor(kind, id)).ok);
  }
  harness.barrier();
  for (auto &message : dispatchFixtures()) {
    const auto kind = callbackKind(message);
    if (!harness.abi->postCallback(std::move(message)).ok) {
      throw TestFailure("callback rejected: " +
                        std::string(callbackKindName(kind)));
    }
  }
  harness.barrier();
  CHECK(harness.abi->resources().liveBridgeCorrelations == 0);
  CHECK(delivered[JsCallbackKind::HandlerRegistrationResult] == 2);
  CHECK(delivered[JsCallbackKind::AppContext] == 1);
  CHECK(callbackOnExecutor);
  CHECK(appContextOrder == std::vector<std::string>{"pkg"});
  CHECK(delivered[JsCallbackKind::SurfaceStatusChanged] == 1);

  auto appOne = dispatchFixtures()[1];
  auto appTwo = dispatchFixtures()[1];
  std::get<AppContext>(appOne).packageId = "pkg-one";
  std::get<AppContext>(appTwo).packageId = "pkg-two";
  CHECK(harness.abi->postCallback(std::move(appOne)).ok);
  CHECK(harness.abi->postCallback(std::move(appTwo)).ok);
  harness.barrier();
  CHECK(appContextOrder ==
        std::vector<std::string>({"pkg", "pkg-one", "pkg-two"}));

  CHECK(harness.abi->postCallback(
            resultFor(JsCallbackKind::ShowToastResult, "req:j-6"))
            .ok);
  harness.barrier();
  CHECK(delivered[JsCallbackKind::ShowToastResult] == 1);

  auto invalidCallback = resultFor(JsCallbackKind::ShowToastResult, "req:j-99");
  std::get<ShowToastResult>(invalidCallback).status = "unknown";
  CHECK(!harness.abi->postCallback(std::move(invalidCallback)).ok);

  const auto oldAppContextCount = delivered[JsCallbackKind::AppContext];
  CHECK(harness.onExecutor([&](JsEnginePort &, const JsContextRef &) {
    return harness.abi->unregisterConsumersOnExecutor(harness.consumerToken);
  }));
  auto appDispatch = dispatchFixtures();
  CHECK(harness.abi->postCallback(std::move(appDispatch[1])).ok);
  harness.barrier();
  CHECK(delivered[JsCallbackKind::AppContext] == oldAppContextCount);

  CHECK(harness.onExecutor([&](JsEnginePort &, const JsContextRef &) {
    return harness.abi->closeSurfaceOnExecutor("srf:1").ok();
  }));
  CHECK(harness.onExecutor([&](JsEnginePort &, const JsContextRef &) {
    return harness.abi->closeSurfaceOnExecutor("srf:1").ok();
  }));
  CHECK(!enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-30")})));
  CHECK(!harness.abi
             ->postCallback(
                 resultFor(JsCallbackKind::ShowToastResult, "req:j-30"))
             .ok);

  harness.stop();
  const auto resources = harness.abi->resources();
  CHECK(resources.liveNativeEntries == 0);
  CHECK(resources.liveBridgeCorrelations == 0);
  CHECK(resources.liveConsumerRegistrations == 0);
  CHECK(resources.openSurfaceScopes == 0);
  CHECK(resources.queuedAbiCallbacks == 0);
}

void verifyQuickJsStrictValues() {
  Harness harness(std::make_unique<QuickJsEngineProvider>());
  harness.startAbi();
  harness.openSurface();
  const auto getter = harness.evaluate(R"JS(
    (() => {
      globalThis.__getterHit = false;
      const message = {
        schemaVersion: 1,
        kind: 'showToast',
        requestId: 'req:j-1',
        surfaceId: 'srf:1',
        durationMs: 1
      };
      Object.defineProperty(message, 'message', {
        enumerable: true,
        get() { globalThis.__getterHit = true; return 'unsafe'; }
      });
      const result = $quickapp_runtime_v1_showToast$(message);
      return {ok: result.ok, getterHit: globalThis.__getterHit};
    })()
  )JS");
  const auto &getterObject = std::get<RuntimeValue::Object>(getter.storage());
  CHECK(!std::get<bool>(getterObject.at("ok").storage()));
  CHECK(!std::get<bool>(getterObject.at("getterHit").storage()));

  const auto proxy = harness.evaluate(R"JS(
    (() => {
      globalThis.__proxyHit = false;
      const target = {
        schemaVersion: 1,
        kind: 'showToast',
        requestId: 'req:j-2',
        surfaceId: 'srf:1',
        message: 'unsafe',
        durationMs: 1
      };
      const message = new Proxy(target, {
        ownKeys(value) { globalThis.__proxyHit = true; return Reflect.ownKeys(value); }
      });
      const result = $quickapp_runtime_v1_showToast$(message);
      return {ok: result.ok, proxyHit: globalThis.__proxyHit};
    })()
  )JS");
  const auto &proxyObject = std::get<RuntimeValue::Object>(proxy.storage());
  CHECK(!std::get<bool>(proxyObject.at("ok").storage()));
  CHECK(!std::get<bool>(proxyObject.at("proxyHit").storage()));

  const auto forbidden = harness.evaluate(R"JS(
    (() => {
      const message = {
        schemaVersion: 1,
        kind: 'showToast',
        requestId: 'req:j-3',
        surfaceId: 'srf:1',
        message: undefined,
        durationMs: NaN
      };
      return $quickapp_runtime_v1_showToast$(message);
    })()
  )JS");
  CHECK(!enqueueOk(forbidden));
  CHECK(harness.core.messageCount() == 0);
  harness.stop();
}

void verifyObservationEquivalence() {
  Harness recording(std::make_unique<FakeEngineProvider>(), 10, 16, false);
  Harness noop(std::make_unique<FakeEngineProvider>(), 10, 16, true);
  recording.startAbi();
  noop.startAbi();
  recording.openSurface();
  noop.openSurface();
  const auto recordingResult = recording.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-1")});
  const auto noopResult = noop.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-1")});
  CHECK(recordingResult == noopResult);
  CHECK(recording.abi->resources().liveBridgeCorrelations ==
        noop.abi->resources().liveBridgeCorrelations);
  CHECK(recording.core.messageCount() == noop.core.messageCount());
  CHECK(recording.recordingSink && recording.recordingSink->size() > 0);
  recording.stop();
  noop.stop();
  CHECK(recording.abi->resources().liveBridgeCorrelations == 0);
  CHECK(noop.abi->resources().liveBridgeCorrelations == 0);
}

void verifyIdentityFailure() {
  auto provider = std::make_unique<FakeEngineProvider>();
  Harness harness(std::move(provider));
  auto result = harness.onExecutor([&](JsEnginePort &port,
                                       const JsContextRef &context) {
    return harness.abi->startOnExecutor(port, context, "wrong-runtime");
  });
  CHECK(!result.ok());
  CHECK(result.error().code == AbiErrorCode::UnsupportedVersion);
  CHECK(harness.abi->resources().liveNativeEntries == 0);
  harness.stop();
}

void verifyPartialBindingRollback() {
  auto provider = std::make_unique<FakeEngineProvider>();
  Harness harness(std::move(provider));
  std::optional<NativeBindingToken> blocker;
  harness.onExecutor([&](JsEnginePort &port, const JsContextRef &context) {
    auto bound = port.bindNativeFunction(
        context,
        NativeFunctionSpec{
            .globalName = "$quickapp_runtime_v1_registerHandler$",
            .minArgs = 1,
            .maxArgs = 1,
            .invoke = [](const NativeCallView &) {
              return NativeFunctionResult::failure(
                  {RuntimeErrorCode::JsException, "blocker"});
            },
        });
    CHECK(bound.ok());
    blocker.emplace(std::move(bound).value());
    return true;
  });
  auto started = harness.onExecutor([&](JsEnginePort &port,
                                        const JsContextRef &context) {
    return harness.abi->startOnExecutor(port, context, kRuntimeAbiIdentity);
  });
  CHECK(!started.ok());
  CHECK(harness.abi->resources().liveNativeEntries == 0);
  harness.onExecutor([&](JsEnginePort &port, const JsContextRef &context) {
    CHECK(blocker && blocker->valid());
    CHECK(port.unbindNativeFunction(context, *blocker).ok());
    return true;
  });
  harness.stop();
}

void verifyCallbackQueueOverflow() {
  auto provider = std::make_unique<FakeEngineProvider>();
  Harness harness(std::move(provider), 10, 4);
  harness.startAbi();
  harness.openSurface();

  std::mutex mutex;
  std::condition_variable enteredCv;
  std::condition_variable releaseCv;
  bool entered = false;
  bool release = false;
  const auto blocker = harness.engine->post(
      [&](JsEnginePort &, const JsContextRef &) {
        std::unique_lock lock(mutex);
        entered = true;
        enteredCv.notify_one();
        releaseCv.wait(lock, [&] { return release; });
      });
  CHECK(blocker.status == PostStatus::Accepted);
  {
    std::unique_lock lock(mutex);
    enteredCv.wait(lock, [&] { return entered; });
  }
  std::size_t overflow = 0;
  for (int i = 0; i < 16; ++i) {
    auto fixtures = dispatchFixtures();
    const auto result = harness.abi->postCallback(std::move(fixtures[1]));
    if (!result.ok && result.error &&
        result.error->code == AbiErrorCode::QueueOverflow) {
      ++overflow;
    }
  }
  CHECK(overflow > 0);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  releaseCv.notify_one();
  while (harness.engine->executor().pendingDepth() >= 4) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  harness.barrier();
  harness.stop();
  CHECK(harness.abi->resources().queuedAbiCallbacks == 0);
}

void verifyModuleBundleByteOwnership() {
  {
    Harness harness(std::make_unique<FakeEngineProvider>());
    harness.startAbi();
    harness.openSurface();
    const std::vector<std::uint8_t> *expectedStorage = nullptr;
    bool delivered = false;
    CallbackSlots slots;
    slots.loadVerifiedModule = [&](const LoadVerifiedModule &message) {
      CHECK(message.bundle.bytes);
      CHECK(message.bundle.bytes.get() == expectedStorage);
      CHECK(message.bundle.bytes->size() == 1);
      CHECK(message.bundle.bytes->front() == static_cast<std::uint8_t>('a'));
      delivered = true;
    };
    auto registration = harness.onExecutor(
        [&](JsEnginePort &, const JsContextRef &) {
          return harness.abi->registerConsumersOnExecutor(std::move(slots));
        });
    CHECK(registration.ok());
    harness.consumerToken = std::move(registration).value();

    auto storage = immutableBytes("a");
    std::weak_ptr<const std::vector<std::uint8_t>> released = storage;
    expectedStorage = storage.get();
    JsInboundMessage message = appModuleCallback(std::move(storage));
    CHECK(!storage);
    CHECK(harness.abi->postCallback(std::move(message)).ok);
    CHECK(!released.expired());
    harness.barrier();
    CHECK(delivered);
    CHECK(released.expired());
    harness.stop();
  }

  {
    Harness harness(std::make_unique<FakeEngineProvider>());
    harness.startAbi();
    harness.openSurface();
    auto storage = immutableBytes("a");
    std::weak_ptr<const std::vector<std::uint8_t>> released = storage;
    auto callback = appModuleCallback(std::move(storage));
    callback.bundle.byteLength = 2;
    CHECK(!harness.abi->postCallback(JsInboundMessage{std::move(callback)}).ok);
    CHECK(released.expired());
    harness.stop();
  }

  {
    Harness harness(std::make_unique<FakeEngineProvider>(), 10, 1);
    harness.startAbi();
    harness.openSurface();
    std::mutex mutex;
    std::condition_variable enteredCv;
    std::condition_variable releaseCv;
    bool entered = false;
    bool release = false;
    CHECK(harness.engine
              ->post([&](JsEnginePort &, const JsContextRef &) {
                std::unique_lock lock(mutex);
                entered = true;
                enteredCv.notify_one();
                releaseCv.wait(lock, [&] { return release; });
              })
              .status == PostStatus::Accepted);
    {
      std::unique_lock lock(mutex);
      enteredCv.wait(lock, [&] { return entered; });
    }
    auto appContext = dispatchFixtures()[1];
    CHECK(harness.abi->postCallback(std::move(appContext)).ok);
    auto storage = immutableBytes("a");
    std::weak_ptr<const std::vector<std::uint8_t>> released = storage;
    CHECK(!harness.abi
               ->postCallback(JsInboundMessage{
                   appModuleCallback(std::move(storage))})
               .ok);
    CHECK(released.expired());
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    releaseCv.notify_one();
    while (harness.engine->executor().pendingDepth() >= 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    harness.barrier();
    harness.stop();
  }

  {
    Harness harness(std::make_unique<FakeEngineProvider>());
    harness.startAbi();
    harness.openSurface();
    std::mutex mutex;
    std::condition_variable enteredCv;
    std::condition_variable releaseCv;
    bool entered = false;
    bool release = false;
    CHECK(harness.engine
              ->post([&](JsEnginePort &, const JsContextRef &) {
                std::unique_lock lock(mutex);
                entered = true;
                enteredCv.notify_one();
                releaseCv.wait(lock, [&] { return release; });
              })
              .status == PostStatus::Accepted);
    {
      std::unique_lock lock(mutex);
      enteredCv.wait(lock, [&] { return entered; });
    }
    CHECK(harness.engine
              ->post([runtime = harness.abi](JsEnginePort &,
                                             const JsContextRef &) {
                CHECK(runtime->closeSurfaceOnExecutor("srf:1").ok());
              })
              .status == PostStatus::Accepted);
    auto storage = immutableBytes("a");
    std::weak_ptr<const std::vector<std::uint8_t>> released = storage;
    CHECK(harness.abi
              ->postCallback(JsInboundMessage{
                  pageModuleCallback(std::move(storage))})
              .ok);
    CHECK(!released.expired());
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    releaseCv.notify_one();
    harness.barrier();
    CHECK(released.expired());
    harness.stop();
  }

  {
    Harness harness(std::make_unique<FakeEngineProvider>());
    harness.startAbi();
    harness.openSurface();
    std::mutex mutex;
    std::condition_variable enteredCv;
    std::condition_variable releaseCv;
    bool entered = false;
    bool release = false;
    CHECK(harness.engine
              ->post([&](JsEnginePort &, const JsContextRef &) {
                std::unique_lock lock(mutex);
                entered = true;
                enteredCv.notify_one();
                releaseCv.wait(lock, [&] { return release; });
              })
              .status == PostStatus::Accepted);
    {
      std::unique_lock lock(mutex);
      enteredCv.wait(lock, [&] { return entered; });
    }
    auto storage = immutableBytes("a");
    std::weak_ptr<const std::vector<std::uint8_t>> released = storage;
    CHECK(harness.abi
              ->postCallback(JsInboundMessage{
                  appModuleCallback(std::move(storage))})
              .ok);
    std::promise<void> stopped;
    CHECK(harness.engine->stop(
        [runtime = harness.abi] { runtime->stopOnExecutor(); },
        [&] { stopped.set_value(); }));
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    releaseCv.notify_one();
    stopped.get_future().get();
    CHECK(released.expired());
    CHECK(harness.abi->resources().queuedAbiCallbacks == 0);
  }
}

void verifyCloseRaceAndSingleThreadQueue() {
  Harness harness(std::make_unique<FakeEngineProvider>());
  harness.startAbi();
  harness.openSurface();
  std::atomic<int> delivered{0};
  CallbackSlots slots;
  slots.surfaceStatusChanged =
      [&](const SurfaceStatusChanged &) { delivered.fetch_add(1); };
  auto registration = harness.onExecutor(
      [&](JsEnginePort &, const JsContextRef &) {
        return harness.abi->registerConsumersOnExecutor(std::move(slots));
      });
  CHECK(registration.ok());
  harness.consumerToken = std::move(registration).value();

  const bool queuedWithoutInlineDelivery = harness.onExecutor(
      [&](JsEnginePort &, const JsContextRef &) {
        auto fixture = dispatchFixtures();
        const auto accepted =
            harness.abi->postCallback(std::move(fixture.back())).ok;
        return accepted && delivered.load() == 0;
      });
  CHECK(queuedWithoutInlineDelivery);
  harness.barrier();
  CHECK(delivered.load() == 1);

  std::mutex mutex;
  std::condition_variable enteredCv;
  std::condition_variable releaseCv;
  bool entered = false;
  bool release = false;
  CHECK(harness.engine
            ->post([&](JsEnginePort &, const JsContextRef &) {
              std::unique_lock lock(mutex);
              entered = true;
              enteredCv.notify_one();
              releaseCv.wait(lock, [&] { return release; });
            })
            .status == PostStatus::Accepted);
  {
    std::unique_lock lock(mutex);
    enteredCv.wait(lock, [&] { return entered; });
  }
  CHECK(harness.engine
            ->post([runtime = harness.abi](JsEnginePort &,
                                           const JsContextRef &) {
              CHECK(runtime->closeSurfaceOnExecutor("srf:1").ok());
            })
            .status == PostStatus::Accepted);
  auto fixture = dispatchFixtures();
  CHECK(harness.abi->postCallback(std::move(fixture.back())).ok);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  releaseCv.notify_one();
  harness.barrier();
  CHECK(delivered.load() == 1);
  harness.stop();
}

void verifyStopWithPendingWork() {
  Harness harness(std::make_unique<FakeEngineProvider>());
  harness.startAbi();
  harness.openSurface();
  CHECK(enqueueOk(harness.callBinding(
      bindingName(CoreMessageKind::ShowToast),
      {outbound(CoreMessageKind::ShowToast, "req:j-1")})));
  CHECK(harness.abi->resources().liveBridgeCorrelations == 1);

  std::mutex mutex;
  std::condition_variable enteredCv;
  std::condition_variable releaseCv;
  bool entered = false;
  bool release = false;
  CHECK(harness.engine
            ->post([&](JsEnginePort &, const JsContextRef &) {
              std::unique_lock lock(mutex);
              entered = true;
              enteredCv.notify_one();
              releaseCv.wait(lock, [&] { return release; });
            })
            .status == PostStatus::Accepted);
  {
    std::unique_lock lock(mutex);
    enteredCv.wait(lock, [&] { return entered; });
  }
  auto callbacks = dispatchFixtures();
  CHECK(harness.abi->postCallback(std::move(callbacks[1])).ok);
  std::promise<void> stopped;
  CHECK(harness.engine->stop(
      [runtime = harness.abi] { runtime->stopOnExecutor(); },
      [&] { stopped.set_value(); }));
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  releaseCv.notify_one();
  stopped.get_future().get();
  const auto resources = harness.abi->resources();
  CHECK(resources.liveNativeEntries == 0);
  CHECK(resources.liveBridgeCorrelations == 0);
  CHECK(resources.liveConsumerRegistrations == 0);
  CHECK(resources.openSurfaceScopes == 0);
  CHECK(resources.queuedAbiCallbacks == 0);
}

void verifyCodecCrossFields() {
  auto failed = outbound(CoreMessageKind::CompleteVmInitialization);
  auto &fields = std::get<RuntimeValue::Object>(failed.storage());
  fields["status"] = RuntimeValue("failed");
  fields["failedPhase"] = RuntimeValue("onReady");
  fields["error"] = errorValue();
  CHECK(!decodeCoreMessage(CoreMessageKind::CompleteVmInitialization, failed,
                           ValueLimits{32, 2048})
             .ok());

  auto valid = outbound(CoreMessageKind::CompleteVmInitialization);
  auto &validFields = std::get<RuntimeValue::Object>(valid.storage());
  validFields["status"] = RuntimeValue("failed");
  validFields["failedPhase"] = RuntimeValue("onCreate");
  validFields["error"] = errorValue();
  CHECK(decodeCoreMessage(CoreMessageKind::CompleteVmInitialization, valid,
                          ValueLimits{32, 2048})
            .ok());

  auto optionalMissing = outbound(CoreMessageKind::SetMeta, "req:j-1");
  CHECK(decodeCoreMessage(CoreMessageKind::SetMeta, optionalMissing,
                          ValueLimits{32, 2048})
            .ok());
  auto optionalNull = outbound(CoreMessageKind::SetMeta, "req:j-2");
  auto &optionalNullFields =
      std::get<RuntimeValue::Object>(optionalNull.storage());
  optionalNullFields["title"] = RuntimeValue(nullptr);
  CHECK(!decodeCoreMessage(CoreMessageKind::SetMeta, optionalNull,
                           ValueLimits{32, 2048})
             .ok());

  auto requiredMissing = outbound(CoreMessageKind::ShowToast, "req:j-3");
  std::get<RuntimeValue::Object>(requiredMissing.storage()).erase("message");
  CHECK(!decodeCoreMessage(CoreMessageKind::ShowToast, requiredMissing,
                           ValueLimits{32, 2048})
             .ok());
  auto wrongKind = outbound(CoreMessageKind::ShowToast, "req:j-4");
  std::get<RuntimeValue::Object>(wrongKind.storage())["kind"] =
      RuntimeValue("deviceGetInfo");
  CHECK(!decodeCoreMessage(CoreMessageKind::ShowToast, wrongKind,
                           ValueLimits{32, 2048})
             .ok());

  auto unknownError = resultFor(JsCallbackKind::ShowToastResult, "req:j-5");
  auto &typedError = std::get<ShowToastResult>(unknownError);
  typedError.status = "failed";
  typedError.error = MessageRuntimeError{"NOT_A_RUNTIME_ERROR", "failed",
                                         false, std::nullopt, std::nullopt,
                                         std::nullopt, std::nullopt};
  CHECK(!validateJsInboundMessage(unknownError, ValueLimits{32, 2048}).ok());
}

void verifyTypedMessageExtraction() {
  auto instantiate = outbound(CoreMessageKind::InstantiateTemplate);
  auto &instantiateObject =
      std::get<RuntimeValue::Object>(instantiate.storage());
  instantiateObject["initialBindings"] = RuntimeValue(object({
      {"1", RuntimeValue("headline")}, {"2", RuntimeValue(true)}}));
  instantiateObject["initialHandlers"] = RuntimeValue(RuntimeValue::Array{
      RuntimeValue(object({{"ownerInstanceId", RuntimeValue("cmp:1")},
                           {"templateHandlerId", RuntimeValue(7.0)},
                           {"handlerId", RuntimeValue("hdl:7")}}))});
  instantiateObject["initialBlocks"] = RuntimeValue(RuntimeValue::Array{
      RuntimeValue(object({
          {"kind", RuntimeValue("instantiateBlock")},
          {"templateBlockId", RuntimeValue(3.0)},
          {"blockInstanceId", RuntimeValue("blk:3")},
          {"parent", RuntimeValue(object({
                         {"ownerInstanceId", RuntimeValue("cmp:1")},
                         {"templateNodeId", RuntimeValue(4.0)}}))},
          {"index", RuntimeValue(0.0)},
          {"key", RuntimeValue("item-a")},
          {"initialBindings",
           RuntimeValue(object({{"5", RuntimeValue("row")}}))},
          {"handlers", RuntimeValue(RuntimeValue::Array{
                           RuntimeValue(object({
                               {"ownerInstanceId", RuntimeValue("blk:3")},
                               {"templateHandlerId", RuntimeValue(8.0)},
                               {"handlerId", RuntimeValue("hdl:8")}}))})},
      }))});
  auto decoded = decodeCoreMessage(CoreMessageKind::InstantiateTemplate,
                                   instantiate, ValueLimits{32, 2048});
  CHECK(decoded.ok());
  const auto &typed = std::get<InstantiateTemplate>(decoded.value());
  CHECK(std::get<std::string>(typed.initialBindings.at(1)) == "headline");
  CHECK(std::get<bool>(typed.initialBindings.at(2)));
  CHECK(typed.initialBlocks.at(0).parent.templateNodeId == 4);
  CHECK(std::get<std::string>(*typed.initialBlocks.at(0).key) == "item-a");
  CHECK(typed.initialBlocks.at(0).handlers.at(0).handlerId == "hdl:8");
  CHECK(typed.initialHandlers.at(0).templateHandlerId == 7);

  auto render = outbound(CoreMessageKind::SubmitRenderTransaction);
  auto &renderObject = std::get<RuntimeValue::Object>(render.storage());
  renderObject["operations"] = RuntimeValue(RuntimeValue::Array{
      RuntimeValue(object({{"kind", RuntimeValue("updateBinding")},
                           {"ownerInstanceId", RuntimeValue("cmp:1")},
                           {"templateBindingId", RuntimeValue(9.0)},
                           {"value", RuntimeValue(false)}}))});
  auto decodedRender = decodeCoreMessage(CoreMessageKind::SubmitRenderTransaction,
                                         render, ValueLimits{32, 2048});
  CHECK(decodedRender.ok());
  const auto &operation = std::get<UpdateBindingOperation>(
      std::get<SubmitRenderTransaction>(decodedRender.value()).operations.at(0));
  CHECK(operation.templateBindingId == 9);
  CHECK(!std::get<bool>(operation.value));

  auto navigation = outbound(CoreMessageKind::NavigationPush);
  auto &navigationObject = std::get<RuntimeValue::Object>(navigation.storage());
  navigationObject["params"] =
      RuntimeValue(object({{"query", RuntimeValue("typed")}}));
  auto decodedNavigation = decodeCoreMessage(CoreMessageKind::NavigationPush,
                                             navigation,
                                             ValueLimits{32, 2048});
  CHECK(decodedNavigation.ok());
  const auto &query = std::get<NavigationPush>(decodedNavigation.value())
                          .params.at("query");
  CHECK(std::get<std::string>(query.storage()) == "typed");
}

void run(std::string_view name, const std::function<void()> &test) {
  test();
  std::cout << "PASS " << name << '\n';
}

} // namespace

int main() {
  try {
    run("JS-S02-A24 allocator fixture", verifyAllocatorFixture);
    run("JS-S02 codec cross fields", verifyCodecCrossFields);
    run("JS-S02 typed message extraction", verifyTypedMessageExtraction);
    run("JS-S02 identity failure", verifyIdentityFailure);
    run("JS-S02 partial binding rollback", verifyPartialBindingRollback);
    run("JS-S02 callback queue overflow", verifyCallbackQueueOverflow);
    run("JS-S02 immutable ModuleBundle byte ownership",
        verifyModuleBundleByteOwnership);
    run("JS-S02 close race and queued same-thread callback",
        verifyCloseRaceAndSingleThreadQueue);
    run("JS-S02 stop with pending work", verifyStopWithPendingWork);
    run("JS-S02 observation equivalence", verifyObservationEquivalence);
    run("JS-S02 QuickJS strict values", verifyQuickJsStrictValues);
    run("JS-S02 Fake Engine common ABI", [] {
      runAbiContractSuite(std::make_unique<FakeEngineProvider>());
    });
    run("JS-S02 QuickJS common ABI", [] {
      runAbiContractSuite(std::make_unique<QuickJsEngineProvider>());
    });
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
