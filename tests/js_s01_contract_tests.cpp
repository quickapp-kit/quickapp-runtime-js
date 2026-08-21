#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "quickapp/js/engine/fake_engine_provider.h"
#include "quickapp/js/engine/js_engine_service.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"

namespace {

using namespace quickapp::js;
using quickapp::js::testing::FakeEngineProvider;
using quickapp::js::testing::FakeFunctionBehavior;
using quickapp::js::testing::FakeSourcePlan;

class TestFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                          \
                        std::to_string(__LINE__) +                             \
                        ": CHECK(" #condition ") failed");                     \
    }                                                                          \
  } while (false)

EngineException plannedException(EngineExceptionKind kind,
                                 std::string message) {
  return EngineException{kind,         std::move(message), std::nullopt,
                         std::nullopt, std::nullopt,       std::nullopt};
}

RuntimeValue commonObject() {
  return RuntimeValue::Object{
      {"answer", RuntimeValue(42.0)},
      {"nested", RuntimeValue::Array{RuntimeValue(true), RuntimeValue("ok"),
                                     RuntimeValue(nullptr)}}};
}

std::unique_ptr<FakeEngineProvider> configuredFakeProvider() {
  auto provider = std::make_unique<FakeEngineProvider>();
  provider->setPlan("value", FakeSourcePlan{.result = commonObject()});
  provider->setPlan(
      "sum",
      FakeSourcePlan{.result = RuntimeValue(0.0),
                     .callable = true,
                     .functionBehavior = FakeFunctionBehavior::SumArguments});
  provider->setPlan(
      "thrower",
      FakeSourcePlan{.result = RuntimeValue(nullptr),
                     .callable = true,
                     .functionException = plannedException(
                         EngineExceptionKind::Runtime, "function throw")});
  provider->setPlan("microtasks", FakeSourcePlan{.result = RuntimeValue(0.0),
                                                 .microtasks = 5});
  provider->setPlan("syntax", FakeSourcePlan{.exception = plannedException(
                                                 EngineExceptionKind::Syntax,
                                                 "syntax error")});
  provider->setPlan("runtime", FakeSourcePlan{.exception = plannedException(
                                                  EngineExceptionKind::Runtime,
                                                  "runtime error")});
  provider->setPlan("terminated",
                    FakeSourcePlan{.exception = plannedException(
                                       EngineExceptionKind::Terminated,
                                       "engine terminated")});
  provider->setPlan("number", FakeSourcePlan{.result = RuntimeValue(7.0)});
  return provider;
}

SourceUnit source(std::string id) {
  static const std::map<std::string, std::string, std::less<>> sources{
      {"value", "({answer: 42, nested: [true, 'ok', null]})"},
      {"sum", "(function(a, b) { return a + b; })"},
      {"thrower", "(function() { throw new Error('function throw'); })"},
      {"microtasks",
       "Promise.resolve().then(()=>0); Promise.resolve().then(()=>0); "
       "Promise.resolve().then(()=>0); Promise.resolve().then(()=>0); "
       "Promise.resolve().then(()=>0); 0"},
      {"syntax", "function ("},
      {"runtime", "throw new Error('runtime error')"},
      {"number", "7"},
  };
  return SourceUnit{id, "case://" + id + ".js", sources.at(id),
                    SourceMode::Script};
}

JsEngineConfig configFor(const JsEngineDescriptor &descriptor,
                         std::function<void()> onOutOfMemory = {}) {
  JsEngineConfig config;
  config.expectedEngine = descriptor;
  config.limits.maxHeapBytes = 32ULL * 1024ULL * 1024ULL;
  config.limits.maxStackBytes = 512ULL * 1024ULL;
  config.limits.maxPendingTasks = 64;
  config.limits.maxMicrotasksPerTurn = 2;
  config.limits.maxRuntimeValueDepth = 16;
  config.limits.maxRuntimeValueNodes = 256;
  config.onOutOfMemory = std::move(onOutOfMemory);
  return config;
}

void runEngineContractSuite(std::unique_ptr<JsEngineProvider> provider,
                            std::string_view expectedEngineId) {
  const auto descriptor = provider->describe();
  CHECK(descriptor.engineId == expectedEngineId);
  CHECK(descriptor.engineAbi == "quickapp-kit-js-engine-v1");
  auto port = provider->create(configFor(descriptor));
  CHECK(port != nullptr);
  CHECK(port->describe() == descriptor);

  auto contextResult = port->createContext();
  CHECK(contextResult.ok());
  JsContextRef context = std::move(contextResult).value();

  auto valueResult = port->evaluate(context, source("value"));
  CHECK(valueResult.ok());
  JsValueRef value = std::move(valueResult).value();
  auto decoded = port->toRuntimeValue(context, value, ValueLimits{16, 256});
  CHECK(decoded.ok());
  CHECK(decoded.value() == commonObject());
  CHECK(!port->toRuntimeValue(context, value, ValueLimits{1, 256}).ok());
  CHECK(!port->toRuntimeValue(context, value, ValueLimits{16, 2}).ok());

  auto retainedResult = port->retain(context, value);
  CHECK(retainedResult.ok());
  JsValueRef retained = std::move(retainedResult).value();
  value.reset();
  auto retainedDecoded =
      port->toRuntimeValue(context, retained, ValueLimits{16, 256});
  CHECK(retainedDecoded.ok());
  CHECK(retainedDecoded.value() == commonObject());

  auto globalResult = port->globalObject(context);
  CHECK(globalResult.ok());
  JsValueRef global = std::move(globalResult).value();
  auto propertyValueResult =
      port->fromRuntimeValue(context, RuntimeValue("property"));
  CHECK(propertyValueResult.ok());
  JsValueRef propertyValue = std::move(propertyValueResult).value();
  CHECK(port->setProperty(context, global, "contractProperty", propertyValue)
            .ok());
  auto propertyResult = port->getProperty(context, global, "contractProperty");
  CHECK(propertyResult.ok());
  JsValueRef property = std::move(propertyResult).value();
  auto propertyDecoded =
      port->toRuntimeValue(context, property, ValueLimits{4, 8});
  CHECK(propertyDecoded.ok());
  CHECK(propertyDecoded.value() == RuntimeValue("property"));

  auto functionResult = port->evaluate(context, source("sum"));
  CHECK(functionResult.ok());
  JsValueRef function = std::move(functionResult).value();
  auto callable = port->isCallable(context, function);
  CHECK(callable.ok() && callable.value());
  auto arg1Result = port->fromRuntimeValue(context, RuntimeValue(20.0));
  auto arg2Result = port->fromRuntimeValue(context, RuntimeValue(22.0));
  CHECK(arg1Result.ok() && arg2Result.ok());
  std::vector<JsValueRef> args;
  args.push_back(std::move(arg1Result).value());
  args.push_back(std::move(arg2Result).value());
  auto callResult = port->call(context, function, global, args);
  CHECK(callResult.ok());
  JsValueRef callValue = std::move(callResult).value();
  auto callDecoded =
      port->toRuntimeValue(context, callValue, ValueLimits{4, 8});
  CHECK(callDecoded.ok());
  CHECK(callDecoded.value() == RuntimeValue(42.0));

  auto throwingFunctionResult = port->evaluate(context, source("thrower"));
  CHECK(throwingFunctionResult.ok());
  JsValueRef throwingFunction = std::move(throwingFunctionResult).value();
  auto thrownCall = port->call(context, throwingFunction, global, {});
  CHECK(!thrownCall.ok());
  CHECK(thrownCall.error().kind == EngineExceptionKind::Runtime);
  auto healthyCall = port->call(context, function, global, args);
  CHECK(healthyCall.ok());
  std::move(healthyCall).value().reset();

  bool nativeCalled = false;
  NativeFunctionSpec nativeSpec{
      .globalName = "nativeEcho",
      .minArgs = 1,
      .maxArgs = 1,
      .invoke = [&](const NativeCallView &call) -> NativeFunctionResult {
        nativeCalled = true;
        CHECK(call.thisValue.valid());
        CHECK(call.args.size() == 1);
        auto retainedArgument = port->retain(call.context, call.args.front());
        if (!retainedArgument.ok()) {
          return NativeFunctionResult::failure(RuntimeError{
              RuntimeErrorCode::JsException, retainedArgument.error().message});
        }
        return NativeFunctionResult::success(
            std::move(retainedArgument).value());
      },
  };
  auto tokenResult = port->bindNativeFunction(context, nativeSpec);
  CHECK(tokenResult.ok());
  NativeBindingToken token = std::move(tokenResult).value();
  CHECK(!port->bindNativeFunction(context, nativeSpec).ok());
  auto nativeFunctionResult = port->getProperty(context, global, "nativeEcho");
  CHECK(nativeFunctionResult.ok());
  JsValueRef nativeFunction = std::move(nativeFunctionResult).value();
  auto nativeCall = port->call(context, nativeFunction, global,
                               std::span<const JsValueRef>(args.data(), 1));
  CHECK(nativeCall.ok());
  CHECK(nativeCalled);
  JsValueRef nativeValue = std::move(nativeCall).value();
  auto nativeDecoded =
      port->toRuntimeValue(context, nativeValue, ValueLimits{4, 8});
  CHECK(nativeDecoded.ok());
  CHECK(nativeDecoded.value() == RuntimeValue(20.0));
  CHECK(port->unbindNativeFunction(context, token).ok());
  CHECK(!token.valid());
  CHECK(!port->unbindNativeFunction(context, token).ok());
  nativeCalled = false;
  CHECK(!port->call(context, nativeFunction, global,
                    std::span<const JsValueRef>(args.data(), 1))
             .ok());
  CHECK(!nativeCalled);

  auto microtaskValue = port->evaluate(context, source("microtasks"));
  CHECK(microtaskValue.ok());
  std::uint32_t jobs = 0;
  bool yielded = false;
  for (;;) {
    auto drain = port->drainMicrotasks(context, 2);
    CHECK(drain.ok());
    CHECK(drain.value().jobsExecuted <= 2);
    jobs += drain.value().jobsExecuted;
    yielded = yielded || drain.value().pending;
    if (!drain.value().pending) {
      break;
    }
  }
  CHECK(jobs == 5);
  CHECK(yielded);

  auto syntax = port->evaluate(context, source("syntax"));
  CHECK(!syntax.ok());
  CHECK(syntax.error().kind == EngineExceptionKind::Syntax);
  auto runtime = port->evaluate(context, source("runtime"));
  CHECK(!runtime.ok());
  CHECK(runtime.error().kind == EngineExceptionKind::Runtime);
  auto afterFailure = port->evaluate(context, source("number"));
  CHECK(afterFailure.ok());

  auto nonCallable = port->call(context, propertyValue, global, {});
  CHECK(!nonCallable.ok());
  auto afterBadCall = port->evaluate(context, source("number"));
  CHECK(afterBadCall.ok());

  auto unsafe = port->fromRuntimeValue(
      context, RuntimeValue(static_cast<double>(kMaxWireSafeInteger) + 2.0));
  CHECK(!unsafe.ok());
  auto infinity = port->fromRuntimeValue(
      context, RuntimeValue(std::numeric_limits<double>::infinity()));
  CHECK(!infinity.ok());
  auto releasedResult =
      port->fromRuntimeValue(context, RuntimeValue("released"));
  CHECK(releasedResult.ok());
  auto releasedValue = std::move(releasedResult).value();
  releasedValue.reset();
  CHECK(!port->toRuntimeValue(context, releasedValue, ValueLimits{4, 8}).ok());

  auto gc = port->requestGarbageCollection(context);
  auto memory = port->snapshotMemory(context);
  CHECK(gc.ok() && memory.ok());

  EngineExceptionKind wrongThreadKind = EngineExceptionKind::Syntax;
  std::thread wrongThread([&] {
    auto wrong = port->snapshotMemory(context);
    CHECK(!wrong.ok());
    wrongThreadKind = wrong.error().kind;
  });
  wrongThread.join();
  CHECK(wrongThreadKind == EngineExceptionKind::Runtime);

  retained.reset();
  global.reset();
  propertyValue.reset();
  property.reset();
  function.reset();
  throwingFunction.reset();
  args.clear();
  callValue.reset();
  nativeFunction.reset();
  nativeValue.reset();
  std::move(microtaskValue).value().reset();
  std::move(afterFailure).value().reset();
  std::move(afterBadCall).value().reset();
  CHECK(port->destroyContext(context).ok());
  CHECK(!context.valid());
  CHECK(!port->destroyContext(context).ok());
}

void testEngineContractSuite() {
  runEngineContractSuite(configuredFakeProvider(), "fake");
  runEngineContractSuite(std::make_unique<QuickJsEngineProvider>(), "quickjs");
}

void runContextLeakReconciliation(std::unique_ptr<JsEngineProvider> provider) {
  auto port = provider->create(configFor(provider->describe()));
  CHECK(port);
  auto contextResult = port->createContext();
  CHECK(contextResult.ok());
  auto context = std::move(contextResult).value();
  auto valueResult = port->fromRuntimeValue(context, RuntimeValue("live"));
  CHECK(valueResult.ok());
  auto liveValue = std::move(valueResult).value();
  auto destroyResult = port->destroyContext(context);
  CHECK(!destroyResult.ok());
  CHECK(!context.valid());
  CHECK(!liveValue.valid());
  liveValue.reset();
}

void testContextLeakReconciliation() {
  runContextLeakReconciliation(configuredFakeProvider());
  runContextLeakReconciliation(std::make_unique<QuickJsEngineProvider>());
}

void testQuickJsPureDataRejection() {
  QuickJsEngineProvider provider;
  auto port = provider.create(configFor(provider.describe()));
  CHECK(port != nullptr);
  auto contextResult = port->createContext();
  CHECK(contextResult.ok());
  auto context = std::move(contextResult).value();
  const std::vector<std::string> invalidSources{
      "undefined",
      "(()=>0)",
      "Symbol('x')",
      "1n",
      "NaN",
      "Infinity",
      "(()=>{const x={};x.self=x;return x;})()",
      "Object.defineProperty({},'x',{enumerable:true,get(){return 1}})",
      "new Proxy({x:1},{})",
      "Object.create({inherited: 1})"};
  for (std::size_t index = 0; index < invalidSources.size(); ++index) {
    SourceUnit unit{"invalid-" + std::to_string(index), "case://invalid.js",
                    invalidSources[index], SourceMode::Script};
    auto valueResult = port->evaluate(context, unit);
    CHECK(valueResult.ok());
    auto value = std::move(valueResult).value();
    auto decoded = port->toRuntimeValue(context, value, ValueLimits{16, 128});
    CHECK(!decoded.ok());
  }
  CHECK(port->destroyContext(context).ok());
}

void testNativeFailureIsolation() {
  QuickJsEngineProvider provider;
  auto port = provider.create(configFor(provider.describe()));
  auto contextResult = port->createContext();
  CHECK(port && contextResult.ok());
  auto context = std::move(contextResult).value();
  auto globalResult = port->globalObject(context);
  CHECK(globalResult.ok());
  auto global = std::move(globalResult).value();

  NativeFunctionSpec throwing{
      .globalName = "nativeThrow",
      .invoke = [](const NativeCallView &) -> NativeFunctionResult {
        throw std::runtime_error("native throw");
      },
  };
  auto tokenResult = port->bindNativeFunction(context, throwing);
  CHECK(tokenResult.ok());
  auto token = std::move(tokenResult).value();
  auto functionResult = port->getProperty(context, global, "nativeThrow");
  CHECK(functionResult.ok());
  auto function = std::move(functionResult).value();
  auto failed = port->call(context, function, global, {});
  CHECK(!failed.ok());
  CHECK(failed.error().kind == EngineExceptionKind::NativeBinding);
  auto healthy =
      port->evaluate(context, SourceUnit{"healthy", "case://healthy.js", "9",
                                         SourceMode::Script});
  CHECK(healthy.ok());
  CHECK(port->unbindNativeFunction(context, token).ok());
  function.reset();
  global.reset();
  std::move(healthy).value().reset();
  CHECK(port->destroyContext(context).ok());
}

void runCrossServiceRejection(
    std::unique_ptr<JsEngineProvider> firstProvider,
    std::unique_ptr<JsEngineProvider> secondProvider) {
  auto first = firstProvider->create(configFor(firstProvider->describe()));
  auto second = secondProvider->create(configFor(secondProvider->describe()));
  CHECK(first && second);
  auto firstContextResult = first->createContext();
  auto secondContextResult = second->createContext();
  CHECK(firstContextResult.ok() && secondContextResult.ok());
  auto firstContext = std::move(firstContextResult).value();
  auto secondContext = std::move(secondContextResult).value();
  auto firstValueResult =
      first->fromRuntimeValue(firstContext, RuntimeValue(1.0));
  CHECK(firstValueResult.ok());
  auto firstValue = std::move(firstValueResult).value();
  CHECK(!first->toRuntimeValue(secondContext, firstValue, ValueLimits{4, 8})
             .ok());
  CHECK(!second->toRuntimeValue(secondContext, firstValue, ValueLimits{4, 8})
             .ok());
  firstValue.reset();
  CHECK(first->destroyContext(firstContext).ok());
  CHECK(second->destroyContext(secondContext).ok());
}

void testCrossServiceRejection() {
  runCrossServiceRejection(configuredFakeProvider(), configuredFakeProvider());
  runCrossServiceRejection(std::make_unique<QuickJsEngineProvider>(),
                           std::make_unique<QuickJsEngineProvider>());
}

void testNativeRuntimeErrorCode() {
  QuickJsEngineProvider provider;
  auto port = provider.create(configFor(provider.describe()));
  CHECK(port);
  auto contextResult = port->createContext();
  CHECK(contextResult.ok());
  auto context = std::move(contextResult).value();
  NativeFunctionSpec failing{
      .globalName = "nativeFail",
      .invoke = [](const NativeCallView &) -> NativeFunctionResult {
        return NativeFunctionResult::failure(RuntimeError{
            RuntimeErrorCode::AbiInvalidArgument, "expected invalid argument"});
      },
  };
  auto tokenResult = port->bindNativeFunction(context, failing);
  CHECK(tokenResult.ok());
  auto token = std::move(tokenResult).value();
  auto codeResult = port->evaluate(
      context, SourceUnit{"native-code", "case://native-code.js",
                          "(()=>{try{nativeFail();return "
                          "'missing';}catch(e){return e.code;}})()",
                          SourceMode::Script});
  CHECK(codeResult.ok());
  auto codeValue = std::move(codeResult).value();
  auto decoded = port->toRuntimeValue(context, codeValue, ValueLimits{4, 8});
  CHECK(decoded.ok());
  CHECK(decoded.value() == RuntimeValue("ABI_INVALID_ARGUMENT"));
  auto unrelatedFailure = port->evaluate(
      context, SourceUnit{"after-native-code", "case://after-native-code.js",
                          "throw new Error('unrelated')", SourceMode::Script});
  CHECK(!unrelatedFailure.ok());
  CHECK(unrelatedFailure.error().kind == EngineExceptionKind::Runtime);
  CHECK(port->unbindNativeFunction(context, token).ok());
  codeValue.reset();
  CHECK(port->destroyContext(context).ok());
}

void testRepeatedQuickJsTeardown() {
  QuickJsEngineProvider provider;
  for (int iteration = 0; iteration < 50; ++iteration) {
    auto port = provider.create(configFor(provider.describe()));
    CHECK(port);
    auto contextResult = port->createContext();
    CHECK(contextResult.ok());
    auto context = std::move(contextResult).value();
    auto valueResult = port->evaluate(context, source("value"));
    CHECK(valueResult.ok());
    auto value = std::move(valueResult).value();
    auto retainedResult = port->retain(context, value);
    CHECK(retainedResult.ok());
    auto retained = std::move(retainedResult).value();
    value.reset();
    retained.reset();
    CHECK(port->destroyContext(context).ok());
  }
}

void testQuickJsHeapLimit() {
  QuickJsEngineProvider provider;
  std::atomic<int> oomCount{0};
  auto config = configFor(provider.describe(), [&] { ++oomCount; });
  config.limits.maxHeapBytes = 2ULL * 1024ULL * 1024ULL;
  auto port = provider.create(config);
  CHECK(port);
  auto contextResult = port->createContext();
  CHECK(contextResult.ok());
  auto context = std::move(contextResult).value();
  auto result = port->evaluate(context, SourceUnit{"oom", "case://oom.js",
                                                   "new Array(1000000).fill(0)",
                                                   SourceMode::Script});
  CHECK(!result.ok());
  CHECK(result.error().kind == EngineExceptionKind::OutOfMemory);
  CHECK(oomCount.load() > 0);
  CHECK(port->destroyContext(context).ok());
}

void testManualExecutor() {
  JsExecutor executor(2);
  CHECK(executor.start(ExecutorMode::ManualPump));
  std::vector<int> order;
  auto first = executor.postUniqueMicrotaskContinuation(
      ExecutorTask{.run = [&] { order.push_back(1); }});
  auto duplicate = executor.postUniqueMicrotaskContinuation(
      ExecutorTask{.run = [&] { order.push_back(99); }});
  CHECK(first.status == PostStatus::Accepted);
  CHECK(duplicate.status == PostStatus::Accepted);
  CHECK(first.sequence == duplicate.sequence);
  CHECK(executor.hasPendingMicrotaskContinuation());
  auto second = executor.post(ExecutorTask{.run = [&] { order.push_back(2); }});
  CHECK(second.status == PostStatus::Accepted);
  auto overflow =
      executor.post(ExecutorTask{.run = [&] { order.push_back(3); }});
  CHECK(overflow.status == PostStatus::QueueOverflow);
  executor.pumpUntilIdle();
  CHECK((order == std::vector<int>{1, 2}));
  CHECK(!executor.hasPendingMicrotaskContinuation());

  int cancelled = 0;
  int barrier = 0;
  CHECK(executor
            .post(ExecutorTask{.run = [&] { order.push_back(4); },
                               .onCancelled = [&] { ++cancelled; }})
            .status == PostStatus::Accepted);
  CHECK(executor.beginStop([&] { ++barrier; }));
  CHECK(executor.post(ExecutorTask{}).status == PostStatus::Stopping);
  executor.pumpUntilIdle();
  CHECK(cancelled == 1);
  CHECK(barrier == 1);
  CHECK(executor.state() == ExecutorState::Stopped);
}

void testConcurrentExecutorFifo() {
  JsExecutor executor(512);
  CHECK(executor.start(ExecutorMode::OwnedThread));
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::pair<std::uint64_t, int>> accepted;
  std::vector<int> executed;
  std::atomic<int> nextPayload{0};
  std::vector<std::thread> producers;
  for (int producer = 0; producer < 4; ++producer) {
    producers.emplace_back([&] {
      for (int item = 0; item < 50; ++item) {
        const int payload = nextPayload.fetch_add(1);
        auto result = executor.post(ExecutorTask{.run = [&, payload] {
          std::lock_guard lock(mutex);
          executed.push_back(payload);
          cv.notify_all();
        }});
        CHECK(result.status == PostStatus::Accepted);
        std::lock_guard lock(mutex);
        accepted.emplace_back(result.sequence, payload);
      }
    });
  }
  for (auto &producer : producers) {
    producer.join();
  }
  {
    std::unique_lock lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(5),
                      [&] { return executed.size() == accepted.size(); }));
  }
  std::sort(accepted.begin(), accepted.end());
  std::vector<int> expected;
  expected.reserve(accepted.size());
  for (const auto &[sequence, payload] : accepted) {
    static_cast<void>(sequence);
    expected.push_back(payload);
  }
  CHECK(executed == expected);
  std::promise<void> stopped;
  CHECK(executor.beginStop([] {}, [&] { stopped.set_value(); }));
  CHECK(stopped.get_future().wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready);
}

class FakeClock final : public MonotonicClock {
public:
  std::uint64_t nowNs() const noexcept override { return now.load(); }
  std::atomic<std::uint64_t> now{1000};
};

struct CapturedEvent {
  std::string marker;
  std::uint64_t timestamp{0};
  std::uint64_t sequence{0};
  std::optional<std::uint64_t> value;
};

class RecordingSink final : public TraceSink {
public:
  explicit RecordingSink(std::size_t capacity) : capacity_(capacity) {}
  void emit(const TraceEvent &event) noexcept override {
    if (!enabled_ || events_.size() >= capacity_) {
      return;
    }
    try {
      events_.push_back(CapturedEvent{std::string(event.markerName),
                                      event.timestampNs, event.sequence,
                                      event.counterValue});
    } catch (...) {
      std::terminate();
    }
  }
  void close() noexcept { enabled_ = false; }
  const std::vector<CapturedEvent> &events() const noexcept { return events_; }

private:
  std::size_t capacity_;
  bool enabled_{true};
  std::vector<CapturedEvent> events_;
};

TraceSinkRegistration admittedSink(TraceSink &sink) {
  auto registration = TraceSinkRegistration::admit(
      sink, TraceSinkContractDeclaration{true, true});
  CHECK(registration.ok());
  return std::move(registration).value();
}

void testObservationContract() {
  static_assert(noexcept(
      std::declval<TraceSink &>().emit(std::declval<const TraceEvent &>())));
  FakeClock clock;
  RecordingSink sink(2);
  auto mayBlock = TraceSinkRegistration::admit(
      sink, TraceSinkContractDeclaration{false, true});
  CHECK(!mayBlock.ok());
  CHECK(mayBlock.error() == TraceSinkAdmissionError::MayBlock);
  auto mayReenter = TraceSinkRegistration::admit(
      sink, TraceSinkContractDeclaration{true, false});
  CHECK(!mayReenter.ok());
  CHECK(mayReenter.error() == TraceSinkAdmissionError::MayReenter);
  ObservationEmitter emitter(clock, sink,
                             ObservationConfig{true, "run-1", "steady", 900});
  CHECK(emitter.emitQueueDepth(3) == ObservationEmitResult::Emitted);
  clock.now = 1050;
  CHECK(emitter.emitQueueOverflow(4) == ObservationEmitResult::Emitted);
  sink.close();
  CHECK(emitter.emitOutOfMemory() == ObservationEmitResult::Emitted);
  CHECK(sink.events().size() == 2);
  CHECK(sink.events()[0].timestamp == 100);
  CHECK(sink.events()[1].timestamp == 150);
  CHECK(sink.events()[0].sequence == 0 && sink.events()[1].sequence == 1);

  RecordingSink overflowSink(1);
  clock.now = kMaxWireSafeInteger + 901;
  ObservationEmitter overflowEmitter(
      clock, overflowSink, ObservationConfig{true, "run-2", "steady", 900});
  CHECK(overflowEmitter.emitQueueDepth(1) ==
        ObservationEmitResult::WireIntegerOutOfRange);
  CHECK(overflowEmitter.rotationRequired());
  CHECK(overflowEmitter.emitQueueDepth(1) ==
        ObservationEmitResult::RunRotationRequired);
}

void testServiceCreationFailuresAndOomObservation() {
  for (const auto options :
       {testing::FakeEngineOptions{.failEngineCreate = true},
        testing::FakeEngineOptions{.failContextCreate = true}}) {
    auto fake = std::make_unique<FakeEngineProvider>(options);
    const auto descriptor = fake->describe();
    FakeClock clock;
    RecordingSink sink(8);
    JsEngineService service(
        "app-failure", std::move(fake), configFor(descriptor), clock,
        admittedSink(sink),
        ObservationConfig{true, "run-failure", "steady", clock.nowNs()});
    std::promise<ServiceResult> completed;
    CHECK(service.start(
        [&](ServiceResult result) { completed.set_value(std::move(result)); }));
    auto result = completed.get_future().get();
    CHECK(!result.ok());
    CHECK(result.error().code == RuntimeErrorCode::OutOfMemory);
    CHECK(service.state() == EngineServiceState::Stopped);
    CHECK(std::any_of(sink.events().begin(), sink.events().end(),
                      [](const CapturedEvent &event) {
                        return event.marker == "runtime.oom";
                      }));
  }
}

struct ServiceScenarioOutcome {
  RuntimeValue value;
  std::uint64_t taskSequence{0};
  EngineServiceState finalState{EngineServiceState::New};

  friend bool operator==(const ServiceScenarioOutcome &,
                         const ServiceScenarioOutcome &) = default;
};

ServiceScenarioOutcome runServiceScenario(TraceSink &sink) {
  auto fake = configuredFakeProvider();
  const auto descriptor = fake->describe();
  FakeClock clock;
  JsEngineService service(
      "app-equivalence", std::move(fake), configFor(descriptor), clock,
      admittedSink(sink),
      ObservationConfig{true, "run-equivalence", "steady", clock.nowNs()});
  std::promise<ServiceResult> started;
  CHECK(service.start(
      [&](ServiceResult result) { started.set_value(std::move(result)); }));
  CHECK(started.get_future().get().ok());
  std::promise<RuntimeValue> evaluated;
  const auto posted =
      service.post([&](JsEnginePort &engine, const JsContextRef &context) {
        auto valueResult = engine.evaluate(context, source("number"));
        CHECK(valueResult.ok());
        auto value = std::move(valueResult).value();
        auto decoded = engine.toRuntimeValue(context, value, ValueLimits{4, 8});
        CHECK(decoded.ok());
        evaluated.set_value(std::move(decoded).value());
      });
  CHECK(posted.status == PostStatus::Accepted);
  ServiceScenarioOutcome outcome{evaluated.get_future().get(), posted.sequence,
                                 EngineServiceState::New};
  std::promise<void> stopped;
  CHECK(service.stop([] {}, [&] { stopped.set_value(); }));
  stopped.get_future().get();
  outcome.finalState = service.state();
  return outcome;
}

void testObservationBehaviorEquivalence() {
  NoopTraceSink noop;
  const auto baseline = runServiceScenario(noop);
  RecordingSink normal(32);
  RecordingSink capacityFull(0);
  RecordingSink dropping(1);
  RecordingSink closed(32);
  closed.close();
  CHECK(runServiceScenario(normal) == baseline);
  CHECK(runServiceScenario(capacityFull) == baseline);
  CHECK(runServiceScenario(dropping) == baseline);
  CHECK(runServiceScenario(closed) == baseline);
}

void testServiceQueueOverflowObservation() {
  auto fake = configuredFakeProvider();
  const auto descriptor = fake->describe();
  auto config = configFor(descriptor);
  config.limits.maxPendingTasks = 1;
  FakeClock clock;
  RecordingSink sink(16);
  JsEngineService service(
      "app-overflow", std::move(fake), std::move(config), clock,
      admittedSink(sink),
      ObservationConfig{true, "run-overflow", "steady", clock.nowNs()});
  std::promise<ServiceResult> started;
  CHECK(service.start(
      [&](ServiceResult result) { started.set_value(std::move(result)); }));
  CHECK(started.get_future().get().ok());

  std::mutex mutex;
  std::condition_variable cv;
  bool blockerStarted = false;
  bool releaseBlocker = false;
  CHECK(service
            .post([&](JsEnginePort &, const JsContextRef &) {
              std::unique_lock lock(mutex);
              blockerStarted = true;
              cv.notify_all();
              cv.wait(lock, [&] { return releaseBlocker; });
            })
            .status == PostStatus::Accepted);
  {
    std::unique_lock lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(5),
                      [&] { return blockerStarted; }));
  }
  std::promise<void> queuedCompleted;
  CHECK(service
            .post([&](JsEnginePort &, const JsContextRef &) {
              queuedCompleted.set_value();
            })
            .status == PostStatus::Accepted);
  CHECK(service.post([](JsEnginePort &, const JsContextRef &) {}).status ==
        PostStatus::QueueOverflow);
  {
    std::lock_guard lock(mutex);
    releaseBlocker = true;
  }
  cv.notify_all();
  queuedCompleted.get_future().get();
  CHECK(std::any_of(sink.events().begin(), sink.events().end(),
                    [](const CapturedEvent &event) {
                      return event.marker == "queue.overflow" &&
                             event.value == 1;
                    }));
  std::promise<void> stopped;
  CHECK(service.stop([] {}, [&] { stopped.set_value(); }));
  stopped.get_future().get();
}

void testServiceLifecycleAndMicrotaskContinuation() {
  auto fake = configuredFakeProvider();
  const auto descriptor = fake->describe();
  FakeClock clock;
  NoopTraceSink sink;
  JsEngineService service(
      "app-1", std::move(fake), configFor(descriptor), clock,
      admittedSink(sink),
      ObservationConfig{false, "run-service", "steady", clock.nowNs()});

  std::promise<ServiceResult> started;
  CHECK(service.start(
      [&](ServiceResult result) { started.set_value(std::move(result)); }));
  auto startResult = started.get_future().get();
  CHECK(startResult.ok());
  CHECK(service.state() == EngineServiceState::Running);
  CHECK(!service.start({}));

  std::promise<void> evaluated;
  CHECK(service
            .post([&](JsEnginePort &engine, const JsContextRef &context) {
              auto result = engine.evaluate(context, source("microtasks"));
              CHECK(result.ok());
              evaluated.set_value();
            })
            .status == PostStatus::Accepted);
  CHECK(evaluated.get_future().wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready);

  std::mutex mutex;
  std::condition_variable cv;
  std::uint32_t jobs = 0;
  bool finished = false;
  bool fairnessTaskPosted = false;
  std::promise<void> fairnessTaskRan;
  auto checkpoint =
      service.checkpointMicrotasks([&](EngineResult<MicrotaskDrain> result) {
        CHECK(result.ok());
        if (result.value().pending && !fairnessTaskPosted) {
          fairnessTaskPosted = true;
          CHECK(service
                    .post([&](JsEnginePort &, const JsContextRef &) {
                      fairnessTaskRan.set_value();
                    })
                    .status == PostStatus::Accepted);
        }
        std::lock_guard lock(mutex);
        jobs += result.value().jobsExecuted;
        if (!result.value().pending) {
          finished = true;
          cv.notify_all();
        }
      });
  CHECK(checkpoint.status == PostStatus::Accepted);
  CHECK(service.checkpointMicrotasks({}).status == PostStatus::Accepted);
  {
    std::unique_lock lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return finished; }));
  }
  CHECK(jobs == 5);
  CHECK(fairnessTaskRan.get_future().wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready);

  std::promise<void> stopped;
  int upperTeardown = 0;
  CHECK(service.stop([&] { ++upperTeardown; }, [&] { stopped.set_value(); }));
  CHECK(!service.stop({}, {}));
  CHECK(stopped.get_future().wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready);
  CHECK(upperTeardown == 1);
  CHECK(service.state() == EngineServiceState::Stopped);
  CHECK(service.post([](JsEnginePort &, const JsContextRef &) {}).status ==
        PostStatus::Stopping);
}

void testServiceAbiMismatch() {
  auto fake = configuredFakeProvider();
  auto config = configFor(fake->describe());
  config.expectedEngine.engineVersion = "wrong";
  FakeClock clock;
  NoopTraceSink sink;
  JsEngineService service(
      "app-mismatch", std::move(fake), std::move(config), clock,
      admittedSink(sink),
      ObservationConfig{false, "run-mismatch", "steady", clock.nowNs()});
  std::promise<ServiceResult> result;
  CHECK(service.start(
      [&](ServiceResult value) { result.set_value(std::move(value)); }));
  auto failure = result.get_future().get();
  CHECK(!failure.ok());
  CHECK(failure.error().code == RuntimeErrorCode::ModuleAbiUnsupported);
  for (int attempt = 0;
       attempt < 100 && service.state() != EngineServiceState::Stopped;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(service.state() == EngineServiceState::Stopped);
}

void testUnrecoverableEngineFailureStopsService() {
  auto fake = configuredFakeProvider();
  const auto descriptor = fake->describe();
  FakeClock clock;
  NoopTraceSink sink;
  JsEngineService service(
      "app-terminated", std::move(fake), configFor(descriptor), clock,
      admittedSink(sink),
      ObservationConfig{false, "run-terminated", "steady", clock.nowNs()});
  std::promise<ServiceResult> started;
  CHECK(service.start(
      [&](ServiceResult result) { started.set_value(std::move(result)); }));
  CHECK(started.get_future().get().ok());

  std::promise<EngineExceptionKind> completed;
  CHECK(service
            .postOperation(
                [](JsEnginePort &engine,
                   const JsContextRef &context) -> EngineResult<void> {
                  auto result = engine.evaluate(
                      context, SourceUnit{"terminated", "case://terminated.js",
                                          "0", SourceMode::Script});
                  if (!result.ok()) {
                    return EngineResult<void>::failure(result.error());
                  }
                  return EngineResult<void>::success();
                },
                [&](const EngineResult<void> &result) {
                  CHECK(!result.ok());
                  completed.set_value(result.error().kind);
                })
            .status == PostStatus::Accepted);
  CHECK(completed.get_future().get() == EngineExceptionKind::Terminated);
  for (int attempt = 0;
       attempt < 100 && service.state() != EngineServiceState::Stopped;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(service.state() == EngineServiceState::Stopped);
  CHECK(service.post([](JsEnginePort &, const JsContextRef &) {}).status ==
        PostStatus::Stopping);
}

using Test = std::pair<const char *, void (*)()>;

} // namespace

int main() {
  const std::vector<Test> tests{
      {"EngineContractSuite", testEngineContractSuite},
      {"ContextLeakReconciliation", testContextLeakReconciliation},
      {"QuickJsPureDataRejection", testQuickJsPureDataRejection},
      {"NativeFailureIsolation", testNativeFailureIsolation},
      {"CrossServiceRejection", testCrossServiceRejection},
      {"NativeRuntimeErrorCode", testNativeRuntimeErrorCode},
      {"RepeatedQuickJsTeardown", testRepeatedQuickJsTeardown},
      {"QuickJsHeapLimit", testQuickJsHeapLimit},
      {"ManualExecutor", testManualExecutor},
      {"ConcurrentExecutorFifo", testConcurrentExecutorFifo},
      {"ObservationContract", testObservationContract},
      {"ServiceCreationFailuresAndOomObservation",
       testServiceCreationFailuresAndOomObservation},
      {"ObservationBehaviorEquivalence", testObservationBehaviorEquivalence},
      {"ServiceQueueOverflowObservation", testServiceQueueOverflowObservation},
      {"ServiceLifecycleAndMicrotaskContinuation",
       testServiceLifecycleAndMicrotaskContinuation},
      {"ServiceAbiMismatch", testServiceAbiMismatch},
      {"UnrecoverableEngineFailureStopsService",
       testUnrecoverableEngineFailureStopsService},
  };
  std::size_t passed = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      ++passed;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &failure) {
      std::cerr << "FAIL " << name << ": " << failure.what() << '\n';
      return 1;
    }
  }
  std::cout << "PASS JS-S01 " << passed << '/' << tests.size() << '\n';
  return 0;
}
