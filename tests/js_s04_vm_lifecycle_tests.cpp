#include "quickapp/js/engine/js_engine_service.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"
#include "quickapp/js/alpha/alpha_page_initialization_stage.h"
#include "quickapp/js/binding/alpha_initial_binding_stage.h"
#include "quickapp/js/framework/static_facade_catalog.h"
#include "quickapp/js/module/module_loader.h"
#include "quickapp/js/page/page_host_control.h"
#include "quickapp/js/render/alpha_initial_transaction_builder.h"
#include "quickapp/js/vm/vm_lifecycle_service.h"

#include <atomic>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

namespace {
using namespace quickapp::js;
using namespace quickapp::js::abi;
using namespace quickapp::js::module;
using namespace quickapp::js::vm;

class Failure final : public std::runtime_error { using std::runtime_error::runtime_error; };
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition    \
                << '\n';                                                       \
      throw Failure("check failed: " #condition);                             \
    }                                                                          \
  } while (false)

class Clock final : public MonotonicClock {
public:
  std::uint64_t nowNs() const noexcept override { return now_.fetch_add(100); }
private:
  mutable std::atomic<std::uint64_t> now_{1000};
};
class Sink final : public TraceSink { public: void emit(const TraceEvent &) noexcept override {} };
class Core final : public CoreIngressPort {
public:
  EnqueueResult post(CoreInboundMessage message) noexcept override {
    try { std::lock_guard lock(mutex_); messages_.push_back(std::move(message)); return EnqueueResult::accepted(); }
    catch (...) { return EnqueueResult::rejected({AbiErrorCode::OutOfMemory, "test core allocation failed", true, {}, {}, {}, {}}); }
  }
  [[nodiscard]] std::vector<CoreInboundMessage> messages() const { std::lock_guard lock(mutex_); return messages_; }
private:
  mutable std::mutex mutex_;
  std::vector<CoreInboundMessage> messages_;
};
class Completion final : public ModuleCompletionPort {
public:
  ModuleEnqueueResult post(const ModuleLoadCompletion &value) noexcept override { try { values.push_back(value); return {ModuleEnqueueStatus::Accepted}; } catch (...) { return {ModuleEnqueueStatus::Closed}; } }
  std::vector<ModuleLoadCompletion> values;
};

class SharedAllocator final : public framework::JsRequestIdAllocatorPort {
public:
  std::string nextRequestId() noexcept override {
    return "req:j-" + std::to_string(next_++);
  }

private:
  std::uint64_t next_{1};
};

class RequestProducer final {
public:
  explicit RequestProducer(framework::JsRequestIdAllocatorPort &allocator)
      : allocator_(allocator) {}
  [[nodiscard]] std::string next() noexcept { return allocator_.nextRequestId(); }

private:
  framework::JsRequestIdAllocatorPort &allocator_;
};

JsEngineConfig config(const JsEngineDescriptor &descriptor) {
  JsEngineConfig result; result.expectedEngine = descriptor;
  result.limits.maxHeapBytes = 32ULL * 1024ULL * 1024ULL;
  result.limits.maxStackBytes = 4ULL * 1024ULL * 1024ULL;
  result.limits.maxPendingTasks = 32; result.limits.maxMicrotasksPerTurn = 16;
  result.limits.maxRuntimeValueDepth = 32; result.limits.maxRuntimeValueNodes = 4096;
  return result;
}
ImmutableByteStorage bytes(std::string_view source) {
  auto value = std::make_shared<std::vector<std::uint8_t>>(source.begin(), source.end());
  return std::const_pointer_cast<const std::vector<std::uint8_t>>(value);
}
LoadVerifiedModule module(std::string requestId, std::string kind, std::string moduleId,
                          std::string scope, std::optional<std::string> surfaceId,
                          std::string source, std::string hash,
                          std::optional<BootstrapExpectation> bootstrap,
                          std::optional<std::vector<std::uint64_t>> bindings,
                          std::optional<std::vector<std::uint64_t>> handlers) {
  const auto storage = bytes(source);
  return LoadVerifiedModule{std::move(requestId), "pkg:1", std::move(kind), std::move(moduleId),
      std::move(scope), std::move(surfaceId), ModuleBundle{"module.js", storage->size(), std::move(hash), storage},
      {}, std::move(bootstrap), std::move(bindings), std::move(handlers)};
}
constexpr std::string_view kApp =
    "$app_define$(\"app\", [], function(require, module, exports) { module.exports = {schemaVersion: 1, kind: \"app\", createAppVm: function(context) { return {}; }}; }); $app_bootstrap$(\"app\", {schemaVersion: 1, kind: \"app\", moduleId: \"app\"});";
constexpr std::string_view kPage =
    "$app_define$(\"pages/index\", [], function(require, module, exports) { const router = require(\"@app-module/system.router\").default; module.exports = {schemaVersion: 1, kind: \"page\", createPageVm: function(context) { return {private: {title: \"Hello\"}, router: router, onInit: function() { this.$page.setTitleBar({text: \"Welcome\"}); this.$page.setMeta({title: \"Case 001\", description: \"Alpha S1\"}); }}; }, bindingEvaluators: {\"1\": function(scope) { return this.private.title; }, \"2\": function(scope) { return true; }}, handlerMethods: {}}; }); $app_bootstrap$(\"pages/index\", {schemaVersion: 1, kind: \"page\", moduleId: \"pages/index\", templateId: \"tpl:pages/index\"});";

template <typename Function> auto onExecutor(JsEngineService &service, Function function) {
  using Return = decltype(function(std::declval<JsEnginePort &>(), std::declval<const JsContextRef &>()));
  auto promise = std::make_shared<std::promise<Return>>(); auto future = promise->get_future();
  CHECK(service.post([promise, function = std::move(function)](JsEnginePort &engine, const JsContextRef &context) mutable {
    try { promise->set_value(function(engine, context)); } catch (...) { promise->set_exception(std::current_exception()); }
  }).status == PostStatus::Accepted);
  return future.get();
}

void run() {
  auto provider = std::make_unique<QuickJsEngineProvider>(); const auto descriptor = provider->describe();
  Clock clock; Sink sink; auto registration = TraceSinkRegistration::admit(sink, {.nonblocking = true, .noReentry = true}); CHECK(registration.ok());
  JsEngineService engine("app:1", std::move(provider), config(descriptor), clock, std::move(registration).value(), ObservationConfig{false, "run:js-s04", "steady", 0});
  std::promise<ServiceResult> started; CHECK(engine.start([&started](ServiceResult result) { started.set_value(std::move(result)); })); CHECK(started.get_future().get().ok());
  Completion completion; Core core; SharedAllocator allocator; RequestProducer producerA(allocator), producerB(allocator);
  CHECK(producerA.next() == "req:j-1"); CHECK(producerB.next() == "req:j-2"); CHECK(producerA.next() == "req:j-3");
  std::unique_ptr<ModuleLoader> loader; std::shared_ptr<RuntimeAbiService> abi;
  std::unique_ptr<framework::StaticFacadeCatalog> facades;
  std::unique_ptr<page::PageHostControlInstaller> pageControls;
  std::unique_ptr<binding::AlphaInitialBindingStage> bindingStage;
  std::unique_ptr<render::AlphaInitialTransactionBuilder> transactionBuilder;
  std::unique_ptr<alpha::AlphaPageInitializationStage> pageStage;
  std::unique_ptr<VmLifecycleService> vm;
  onExecutor(engine, [&](JsEnginePort &js, const JsContextRef &context) {
    facades = std::make_unique<framework::StaticFacadeCatalog>();
    CHECK(facades->startOnExecutor(js, context));
    auto router = facades->resolveOnExecutor("@app-module/system.router");
    CHECK(router.ok());
    auto device = facades->resolveOnExecutor("@app-module/system.device");
    CHECK(device.ok());
    auto defaultExport = js.getProperty(context, router.value(), "default");
    CHECK(defaultExport.ok());
    auto push = js.getProperty(context, defaultExport.value(), "push");
    CHECK(push.ok() && js.isCallable(context, push.value()).ok() &&
          js.isCallable(context, push.value()).value());
    CHECK(!facades->resolveOnExecutor("@app-module/system.unknown").ok());
    loader = std::make_unique<ModuleLoader>(engine, completion, "app:1", "pkg:1",
                                            ModuleLoaderLimits{}, facades.get());
    CHECK(loader->startOnExecutor(js, context));
    abi = std::make_shared<RuntimeAbiService>(engine, core, RuntimeAbiLimits{}, CapabilitySupportSnapshot{}); CHECK(abi->startOnExecutor(js, context, kRuntimeAbiIdentity).ok());
    CHECK(abi->openSurfaceOnExecutor("srf:1").ok());
    pageControls = std::make_unique<page::PageHostControlInstaller>(
        engine, *abi, allocator);
    CHECK(pageControls->startOnExecutor(js, context));
    SourceUnit emptyVmSource{"host-capability-filter", "test://page-vm",
                             "({})", SourceMode::Script};
    auto filteredVm = js.evaluate(context, emptyVmSource);
    CHECK(filteredVm.ok());
    CHECK(pageControls
              ->installOnExecutor(
                  filteredVm.value(),
                  SurfaceContext{"srf:1", "pkg:1", "/", "tpl:pages/index",
                                 {}, {"setTitleBar"},
                                 {320, 240, "logical-px"}})
              .ok());
    auto filteredPage = js.getProperty(context, filteredVm.value(), "$page");
    CHECK(filteredPage.ok());
    auto filteredTitle =
        js.getProperty(context, filteredPage.value(), "setTitleBar");
    auto filteredMeta = js.getProperty(context, filteredPage.value(), "setMeta");
    CHECK(filteredTitle.ok() && js.isCallable(context, filteredTitle.value()).ok() &&
          js.isCallable(context, filteredTitle.value()).value());
    CHECK(filteredMeta.ok() && js.isCallable(context, filteredMeta.value()).ok() &&
          !js.isCallable(context, filteredMeta.value()).value());
    auto slots = loader->callbackSlots();
    bindingStage = std::make_unique<binding::AlphaInitialBindingStage>(engine, *loader);
    transactionBuilder = std::make_unique<render::AlphaInitialTransactionBuilder>(engine, allocator);
    CHECK(bindingStage->startOnExecutor(js, context));
    CHECK(transactionBuilder->startOnExecutor(js, context));
    pageStage = std::make_unique<alpha::AlphaPageInitializationStage>(*bindingStage, *transactionBuilder);
    vm = std::make_unique<VmLifecycleService>(engine, *loader, *pageControls,
                                              *pageStage, "pkg:1"); auto vmSlots = vm->callbackSlots();
    slots.appContext = std::move(vmSlots.appContext); slots.surfaceContext = std::move(vmSlots.surfaceContext); slots.vmInitializationDispatch = std::move(vmSlots.vmInitializationDispatch);
    CHECK(abi->registerConsumersOnExecutor(std::move(slots)).ok()); CHECK(vm->startOnExecutor(js, context));
    loader->onLoadVerifiedModule(module("req:j-1", "app", "app", "appRuntime", std::nullopt, std::string(kApp), "8b70a7994f6240d8de4db434ff135a70fec05d8b132238968f98a6b0ad6e2bf6", BootstrapExpectation{"app", "app", std::nullopt}, std::nullopt, std::nullopt));
    CHECK(loader->openSurfaceOnExecutor("srf:1"));
    loader->onLoadVerifiedModule(module("req:j-2", "page", "pages/index", "surface", "srf:1", std::string(kPage), "8540d6556a9552f5a9dc0ad9f255fbc46850f1d5c298c3e66cdc4a0e7d5eac48", BootstrapExpectation{"page", "pages/index", "tpl:pages/index"}, std::vector<std::uint64_t>{1, 2}, std::vector<std::uint64_t>{}));
    CHECK(completion.values.size() == 2); CHECK(completion.values[0].status == "loaded");
    CHECK(completion.values[1].status == "loaded"); return true;
  });
  onExecutor(engine, [&](JsEnginePort &, const JsContextRef &) {
    vm->onAppContext(AppContext{"pkg:1", "1.0", "1", 1, {}});
    vm->onSurfaceContext(SurfaceContext{"srf:1", "pkg:1", "/", "tpl:pages/index", {}, {"setTitleBar", "setMeta"}, {320, 240, "logical-px"}});
    vm->onVmInitialization(VmInitializationDispatch{"req:3", "app", std::nullopt});
    vm->onVmInitialization(VmInitializationDispatch{"req:4", "page", "srf:1"}); return true;
  });
  const auto messages = core.messages();
  CHECK(messages.size() == 5);
  CHECK(messageKind(messages[0]) == CoreMessageKind::CompleteVmInitialization);
  CHECK(messageKind(messages[1]) == CoreMessageKind::SetTitleBar);
  const auto &title = std::get<SetTitleBar>(messages[1]);
  CHECK(title.requestId == "req:j-4" && title.surfaceId == "srf:1" &&
        title.text == "Welcome");
  CHECK(messageKind(messages[2]) == CoreMessageKind::SetMeta);
  const auto &meta = std::get<SetMeta>(messages[2]);
  CHECK(meta.requestId == "req:j-5" && meta.surfaceId == "srf:1" &&
        meta.title == "Case 001" && meta.description == "Alpha S1");
  CHECK(messageKind(messages[3]) == CoreMessageKind::CompleteVmInitialization);
  const auto &complete = std::get<CompleteVmInitialization>(messages[3]); CHECK(complete.scope == "page"); CHECK(complete.status == "completed");
  CHECK(messageKind(messages[4]) == CoreMessageKind::InstantiateTemplate);
  const auto &instantiate = std::get<InstantiateTemplate>(messages[4]);
  CHECK(instantiate.requestId == "req:j-6");
  CHECK(instantiate.ownerInstanceId == "cmp:srf:1");
  CHECK(std::get<std::string>(instantiate.initialBindings.at(1)) == "Hello");
  CHECK(std::get<bool>(instantiate.initialBindings.at(2)));
  CHECK(onExecutor(engine, [&](JsEnginePort &, const JsContextRef &) { return vm->resources().pageVms == 1; }));
  onExecutor(engine, [&](JsEnginePort &, const JsContextRef &) { vm->closeSurfaceOnExecutor("srf:1"); vm->stopOnExecutor(); transactionBuilder->stopOnExecutor(); bindingStage->stopOnExecutor(); pageControls->stopOnExecutor(); abi->stopOnExecutor(); loader->stopOnExecutor(); facades->stopOnExecutor(); return true; });
  CHECK(onExecutor(engine, [&](JsEnginePort &, const JsContextRef &) { return vm->resources().pageVms == 0 && vm->resources().appVms == 0; }));
  CHECK(pageControls->resources().liveNativeEntries == 0);
  CHECK(pageControls->resources().liveFactoryValues == 0);
  std::promise<void> stopped; CHECK(engine.stop([] {}, [&stopped] { stopped.set_value(); })); stopped.get_future().get();
}
} // namespace
int main() { try { run(); return 0; } catch (const std::exception &error) { return (std::cerr << error.what() << '\n', 1); } }
