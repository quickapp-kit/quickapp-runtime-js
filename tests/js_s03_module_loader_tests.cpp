#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "quickapp/js/engine/js_engine_service.h"
#include "quickapp/js/engine/fake_engine_provider.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"
#include "quickapp/js/module/module_loader.h"

namespace {

using namespace quickapp::js;
using namespace quickapp::js::abi;
using namespace quickapp::js::module;

class TestFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                          \
                        std::to_string(__LINE__) + ": CHECK(" #condition      \
                        ") failed");                                         \
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

class CompletionPort final : public ModuleCompletionPort {
public:
  ModuleEnqueueResult post(const ModuleLoadCompletion &completion) noexcept override {
    if (overflowNext_) {
      overflowNext_ = false;
      return {ModuleEnqueueStatus::QueueOverflow};
    }
    try {
      completions_.push_back(completion);
      return {ModuleEnqueueStatus::Accepted};
    } catch (...) {
      return {ModuleEnqueueStatus::Closed};
    }
  }

  void overflowNext() noexcept { overflowNext_ = true; }
  [[nodiscard]] const std::vector<ModuleLoadCompletion> &completions() const {
    return completions_;
  }

private:
  bool overflowNext_{false};
  std::vector<ModuleLoadCompletion> completions_;
};

JsEngineConfig configFor(const JsEngineDescriptor &descriptor) {
  JsEngineConfig config;
  config.expectedEngine = descriptor;
  config.limits.maxHeapBytes = 32ULL * 1024ULL * 1024ULL;
  config.limits.maxStackBytes = 4ULL * 1024ULL * 1024ULL;
  config.limits.maxPendingTasks = 32;
  config.limits.maxMicrotasksPerTurn = 16;
  config.limits.maxRuntimeValueDepth = 32;
  config.limits.maxRuntimeValueNodes = 4096;
  return config;
}

class LoaderHarness final {
public:
  template <typename Function> auto onExecutor(Function function) {
    using Result = decltype(function(std::declval<JsEnginePort &>(),
                                     std::declval<const JsContextRef &>()));
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    const auto posted = service_->post(
        [promise, function = std::move(function)](JsEnginePort &engine,
                                                   const JsContextRef &context) mutable {
          try {
            promise->set_value(function(engine, context));
          } catch (...) {
            promise->set_exception(std::current_exception());
          }
        });
    CHECK(posted.status == PostStatus::Accepted);
    return future.get();
  }

  explicit LoaderHarness(
      std::unique_ptr<JsEngineProvider> provider =
          std::make_unique<QuickJsEngineProvider>(),
      ModuleLoaderLimits limits = {}) {
    const auto descriptor = provider->describe();
    auto registration = TraceSinkRegistration::admit(
        sink_, {.nonblocking = true, .noReentry = true});
    CHECK(registration.ok());
    service_ = std::make_unique<JsEngineService>(
        "app:1", std::move(provider), configFor(descriptor), clock_,
        std::move(registration).value(),
        ObservationConfig{false, "run:js-s03", "steady", 0});
    std::promise<ServiceResult> started;
    CHECK(service_->start(
        [&started](ServiceResult result) { started.set_value(std::move(result)); }));
    CHECK(started.get_future().get().ok());
    loader_ = std::make_unique<ModuleLoader>(
        *service_, completion_, "app:1", "pkg:1", limits);
    CHECK(onExecutor([this](JsEnginePort &engine, const JsContextRef &context) {
      return loader_->startOnExecutor(engine, context);
    }));
  }

  ~LoaderHarness() noexcept {
    try {
      if (service_ && service_->state() == EngineServiceState::Running) {
        static_cast<void>(onExecutor(
            [this](JsEnginePort &, const JsContextRef &) {
              loader_->stopOnExecutor();
              return true;
            }));
        std::promise<void> stopped;
        if (!service_->stop([] {}, [&stopped] { stopped.set_value(); }))
          std::terminate();
        stopped.get_future().get();
      }
    } catch (...) {
      std::terminate();
    }
  }

  void load(const LoadVerifiedModule &message) {
    static_cast<void>(onExecutor([this, &message](JsEnginePort &,
                                                   const JsContextRef &) {
      loader_->onLoadVerifiedModule(message);
      return true;
    }));
  }

  bool openSurface(std::string surfaceId) {
    return onExecutor([this, surfaceId = std::move(surfaceId)](
                           JsEnginePort &, const JsContextRef &) mutable {
      return loader_->openSurfaceOnExecutor(std::move(surfaceId));
    });
  }

  bool closeSurface(std::string_view surfaceId) {
    return onExecutor([this, surfaceId = std::string(surfaceId)](
                           JsEnginePort &, const JsContextRef &) {
      return loader_->closeSurfaceOnExecutor(surfaceId);
    });
  }

  std::optional<PageModuleLease> pageLease(std::string_view surfaceId,
                                           std::string_view moduleId) {
    return onExecutor(
        [this, surfaceId = std::string(surfaceId),
         moduleId = std::string(moduleId)](JsEnginePort &,
                                            const JsContextRef &) {
          return loader_->pageLeaseOnExecutor(surfaceId, moduleId);
        });
  }

  void stopLoader() {
    static_cast<void>(onExecutor([this](JsEnginePort &, const JsContextRef &) {
      loader_->stopOnExecutor();
      return true;
    }));
  }

  void retry() {
    static_cast<void>(onExecutor([this](JsEnginePort &, const JsContextRef &) {
      loader_->retryCompletionsOnExecutor();
      return true;
    }));
  }

  [[nodiscard]] ModuleResourceSnapshot resources() {
    return onExecutor([this](JsEnginePort &, const JsContextRef &) {
      return loader_->resources();
    });
  }

  CompletionPort completion_;

private:
  TestClock clock_;
  NoopTraceSink sink_;
  std::unique_ptr<JsEngineService> service_;
  std::unique_ptr<ModuleLoader> loader_;
};

ImmutableByteStorage bytesFor(const std::string &source) {
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(source.begin(), source.end());
  return std::const_pointer_cast<const std::vector<std::uint8_t>>(bytes);
}

LoadVerifiedModule moduleMessage(std::string requestId, std::string kind,
                                 std::string moduleId, std::string scope,
                                 std::optional<std::string> surfaceId,
                                 std::string source, std::string sha256,
                                 std::vector<std::string> dependencies = {},
                                 std::optional<BootstrapExpectation> bootstrap =
                                     std::nullopt,
                                 std::optional<std::vector<std::uint64_t>> bindingIds =
                                     std::nullopt,
                                 std::optional<std::vector<std::uint64_t>> handlerIds =
                                     std::nullopt) {
  const auto bytes = bytesFor(source);
  return LoadVerifiedModule{
      std::move(requestId), "pkg:1", std::move(kind), std::move(moduleId),
      std::move(scope), std::move(surfaceId),
      ModuleBundle{"module.js", bytes->size(), std::move(sha256), bytes},
      std::move(dependencies), std::move(bootstrap), std::move(bindingIds),
      std::move(handlerIds)};
}

LoadVerifiedModule appMessage(std::string requestId, std::string source,
                              std::string sha256) {
  return moduleMessage(std::move(requestId), "app", "app", "appRuntime",
                       std::nullopt, std::move(source), std::move(sha256), {},
                       BootstrapExpectation{"app", "app", std::nullopt});
}

constexpr std::string_view kValidApp =
    "$app_define$(\"app\", [], function(require, module, exports) { "
    "module.exports = {schemaVersion: 1, kind: \"app\", createAppVm: "
    "function(context) { return {}; }}; }); "
    "$app_bootstrap$(\"app\", {schemaVersion: 1, kind: \"app\", "
    "moduleId: \"app\"});";

constexpr std::string_view kValidAppSha =
    "8b70a7994f6240d8de4db434ff135a70fec05d8b132238968f98a6b0ad6e2bf6";

testing::FakeSourcePlan fakeValidatorPlan() {
  testing::FakeSourcePlan plan;
  plan.result = RuntimeValue(RuntimeValue::Object{
      {"ok", RuntimeValue(true)},
      {"bindingIds", RuntimeValue(RuntimeValue::Array{})},
      {"handlerIds", RuntimeValue(RuntimeValue::Array{})},
      {"handlerNames", RuntimeValue(RuntimeValue::Object{})}});
  plan.callable = true;
  plan.functionBehavior = testing::FakeFunctionBehavior::ReturnConfiguredData;
  return plan;
}

testing::FakeSourcePlan fakeAppPlan() {
  testing::FakeSourcePlan plan;
  plan.result = RuntimeValue(RuntimeValue::Object{
      {"schemaVersion", RuntimeValue(1.0)},
      {"kind", RuntimeValue("app")},
      {"createAppVm", RuntimeValue(nullptr)}});
  plan.callable = true;
  plan.functionBehavior =
      testing::FakeFunctionBehavior::SetModuleExportsFromConfiguredValue;
  plan.callableProperties = {"createAppVm"};
  plan.nativeCalls.push_back(testing::FakeNativeCall{
      "$app_define$",
      {{RuntimeValue("app"), false},
       {RuntimeValue(RuntimeValue::Array{}), false},
       {RuntimeValue(nullptr), true}}});
  plan.nativeCalls.push_back(testing::FakeNativeCall{
      "$app_bootstrap$",
      {{RuntimeValue("app"), false},
       {RuntimeValue(RuntimeValue::Object{
            {"schemaVersion", RuntimeValue(1.0)},
            {"kind", RuntimeValue("app")},
            {"moduleId", RuntimeValue("app")}}),
        false}}});
  return plan;
}

std::unique_ptr<testing::FakeEngineProvider> configuredFakeProvider() {
  auto provider = std::make_unique<testing::FakeEngineProvider>();
  provider->setPlan("js-s03-definition-validator", fakeValidatorPlan());
  provider->setPlan("module:app", fakeAppPlan());
  return provider;
}

void verifyAppLoadAndCache() {
  LoaderHarness harness;
  harness.load(appMessage("req:j-1", std::string(kValidApp),
                          std::string(kValidAppSha)));
  CHECK(harness.completion_.completions().size() == 1);
  const auto &loaded = harness.completion_.completions().front();
  CHECK(loaded.requestId == "req:j-1");
  CHECK(loaded.status == "loaded");
  CHECK(!loaded.error.has_value());
  const auto afterFirst = harness.resources();
  CHECK(afterFirst.liveEntries == 1);
  CHECK(afterFirst.retainedBytes == 0);

  harness.load(appMessage("req:j-2", std::string(kValidApp),
                          std::string(kValidAppSha)));
  CHECK(harness.completion_.completions().size() == 2);
  CHECK(harness.completion_.completions()[1].status == "loaded");
  CHECK(harness.resources().liveEntries == 1);
}

void verifyFakeAndQuickJsCommonAppLoad() {
  for (auto provider : {0, 1}) {
    std::unique_ptr<JsEngineProvider> engine =
        provider == 0
            ? std::unique_ptr<JsEngineProvider>(configuredFakeProvider())
            : std::make_unique<QuickJsEngineProvider>();
    LoaderHarness harness(std::move(engine));
    harness.load(appMessage("req:j-1", std::string(kValidApp),
                            std::string(kValidAppSha)));
    CHECK(harness.completion_.completions().size() == 1);
    CHECK(harness.completion_.completions()[0].status == "loaded");
    CHECK(harness.resources().liveEntries == 1);
    CHECK(harness.resources().retainedBytes == 0);
  }
}

void verifyIntegrityAndAbiFailure() {
  LoaderHarness harness;
  auto badHash = appMessage("req:j-1", std::string(kValidApp), std::string(64, '0'));
  harness.load(badHash);
  CHECK(harness.completion_.completions().size() == 1);
  CHECK(harness.completion_.completions()[0].error->code ==
        ModuleErrorCode::PackageIntegrityFailed);
  CHECK(harness.resources().liveEntries == 0);

  constexpr std::string_view invalid =
      "$app_define$(\"app\", [], function(require, module, exports) { "
      "module.exports = {schemaVersion: 1, kind: \"app\"}; }); "
      "$app_bootstrap$(\"app\", {schemaVersion: 1, kind: \"app\", "
      "moduleId: \"app\"});";
  auto invalidMessage = appMessage(
      "req:j-2", std::string(invalid),
      "d04ff54ef9a3b4c7fd10a83fc0ecf656c66ea4305e4600b92ac272f880eeadb9");
  harness.load(invalidMessage);
  CHECK(harness.completion_.completions().size() == 2);
  CHECK(harness.completion_.completions()[1].error->code ==
        ModuleErrorCode::ModuleAbiUnsupported);
  CHECK(harness.resources().liveEntries == 1);
  harness.load(appMessage("req:j-3", std::string(invalid),
                          "d04ff54ef9a3b4c7fd10a83fc0ecf656c66ea4305e4600b92ac272f880eeadb9"));
  CHECK(harness.completion_.completions().size() == 3);
  CHECK(harness.completion_.completions()[2].error->code ==
        ModuleErrorCode::ModuleAbiUnsupported);
  CHECK(harness.resources().liveEntries == 1);
}

void verifySharedAndPageScopes() {
  LoaderHarness harness;
  constexpr std::string_view shared =
      "$app_define$(\"shared\", [], function(require, module, exports) { "
      "globalThis.__sharedRuns = (globalThis.__sharedRuns || 0) + 1; "
      "module.exports = {value: 7}; });";
  harness.load(moduleMessage(
      "req:j-1", "shared", "shared", "appRuntime", std::nullopt,
      std::string(shared),
      "22339ab7e2ff9c1acb72035f141fad28ee37fcddef71482ffa6469b9656bab2a"));
  CHECK(harness.completion_.completions().back().status == "loaded");

  constexpr std::string_view app =
      "$app_define$(\"app\", [\"shared\"], function(require, module, exports) { "
      "var shared = require(\"shared\"); module.exports = {schemaVersion: 1, "
      "kind: \"app\", createAppVm: function(context) { return {value: shared.value}; }}; }); "
      "$app_bootstrap$(\"app\", {schemaVersion: 1, kind: \"app\", moduleId: \"app\"});";
  harness.load(moduleMessage(
      "req:j-2", "app", "app", "appRuntime", std::nullopt, std::string(app),
      "f6131341ed4a8d4b1528735f313633132afe3803d18f4dcfea66704ee8cbc3be",
      {"shared"}, BootstrapExpectation{"app", "app", std::nullopt}));
  CHECK(harness.completion_.completions().back().status == "loaded");
  harness.load(moduleMessage(
      "req:j-3", "shared", "shared", "appRuntime", std::nullopt,
      std::string(shared),
      "22339ab7e2ff9c1acb72035f141fad28ee37fcddef71482ffa6469b9656bab2a"));
  CHECK(harness.resources().liveEntries == 2);

  CHECK(harness.openSurface("srf:1"));
  CHECK(harness.openSurface("srf:2"));
  constexpr std::string_view page =
      "$app_define$(\"pages/index\", [], function(require, module, exports) { "
      "module.exports = {schemaVersion: 1, kind: \"page\", createPageVm: function(context) { return {}; }, "
      "bindingEvaluators: {\"1\": function(scope) { return scope.title; }}, "
      "handlerMethods: {\"2\": \"onTap\"}}; }); "
      "$app_bootstrap$(\"pages/index\", {schemaVersion: 1, kind: \"page\", "
      "moduleId: \"pages/index\", templateId: \"tpl:pages/index\"});";
  auto page1 = moduleMessage(
      "req:j-4", "page", "pages/index", "surface", "srf:1", std::string(page),
      "5234f07dd2aaa418d16957935f6430fbc6ec59e64e6e02e02e5c20fb3c20a633",
      {}, BootstrapExpectation{"page", "pages/index", "tpl:pages/index"},
      std::vector<std::uint64_t>{1}, std::vector<std::uint64_t>{2});
  auto page2 = page1;
  page2.requestId = "req:j-5";
  page2.surfaceId = "srf:2";
  harness.load(page1);
  harness.load(page2);
  CHECK(harness.resources().liveEntries == 3);
  CHECK(harness.resources().livePageLeases == 2);
  const auto lease = harness.pageLease("srf:1", "pages/index");
  CHECK(lease.has_value());
  CHECK(lease->surfaceId == "srf:1");
  CHECK(lease->definitionGeneration != 0);
  CHECK(harness.closeSurface("srf:1"));
  CHECK(!harness.pageLease("srf:1", "pages/index").has_value());
  CHECK(harness.resources().livePageLeases == 1);
  CHECK(harness.closeSurface("srf:2"));
  CHECK(harness.resources().livePageLeases == 0);
}

void verifyCompletionRetryAndTeardown() {
  LoaderHarness harness;
  harness.completion_.overflowNext();
  auto message = appMessage("req:j-1", std::string(kValidApp),
                            std::string(kValidAppSha));
  std::weak_ptr<const std::vector<std::uint8_t>> bytes = message.bundle.bytes;
  harness.load(message);
  message.bundle.bytes.reset();
  CHECK(bytes.expired());
  CHECK(harness.completion_.completions().empty());
  CHECK(harness.resources().pendingCompletions == 1);
  harness.retry();
  CHECK(harness.resources().pendingCompletions == 0);
  CHECK(harness.completion_.completions().size() == 1);
  harness.stopLoader();
  const auto released = harness.resources();
  CHECK(released.liveEntries == 0);
  CHECK(released.livePageLeases == 0);
  CHECK(released.activeLoads == 0);
  CHECK(released.retainedBytes == 0);
  CHECK(released.pendingCompletions == 0);
}

void verifyTransientFailureAndLimits() {
  auto provider = configuredFakeProvider();
  auto *providerControl = provider.get();
  providerControl->setPlan(
      "module:app",
      testing::FakeSourcePlan{
          .exception = EngineException{EngineExceptionKind::OutOfMemory,
                                       "injected OOM", std::nullopt,
                                       std::nullopt, std::nullopt,
                                       std::nullopt}});
  LoaderHarness harness(std::move(provider));
  harness.load(appMessage("req:j-1", std::string(kValidApp),
                          std::string(kValidAppSha)));
  CHECK(harness.completion_.completions().back().error->code ==
        ModuleErrorCode::OutOfMemory);
  CHECK(harness.resources().liveEntries == 0);

  providerControl->setPlan("module:app", fakeAppPlan());
  harness.load(appMessage("req:j-2", std::string(kValidApp),
                          std::string(kValidAppSha)));
  CHECK(harness.completion_.completions().back().status == "loaded");
  CHECK(harness.resources().liveEntries == 1);

  LoaderHarness bounded(std::make_unique<QuickJsEngineProvider>(),
                        ModuleLoaderLimits{.maxEntries = 0});
  bounded.load(appMessage("req:j-1", std::string(kValidApp),
                          std::string(kValidAppSha)));
  CHECK(bounded.completion_.completions().back().error->code ==
        ModuleErrorCode::QueueOverflow);
  CHECK(bounded.resources().liveEntries == 0);
  bounded.load(appMessage("req:j-2", std::string(kValidApp),
                          std::string(kValidAppSha)));
  CHECK(bounded.completion_.completions().back().error->code ==
        ModuleErrorCode::QueueOverflow);
  CHECK(bounded.resources().liveEntries == 0);
}

template <typename Function> void run(std::string_view name, Function function) {
  function();
  std::cout << "PASS " << name << '\n';
}

} // namespace

int main() {
  try {
    run("JS-S03 App load and cache", verifyAppLoadAndCache);
    run("JS-S03 Fake and QuickJS common App load",
        verifyFakeAndQuickJsCommonAppLoad);
    run("JS-S03 integrity and ABI failure", verifyIntegrityAndAbiFailure);
    run("JS-S03 Shared require and Page scopes", verifySharedAndPageScopes);
    run("JS-S03 completion retry", verifyCompletionRetryAndTeardown);
    run("JS-S03 transient retry and limits", verifyTransientFailureAndLimits);
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
