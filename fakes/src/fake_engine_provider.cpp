#include "quickapp/js/engine/fake_engine_provider.h"

#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "engine/internal/ref_access.h"

namespace quickapp::js::testing {
namespace {

std::atomic<std::uint64_t> gNextServiceId{1};

EngineException error(EngineExceptionKind kind, std::string message) {
  return EngineException{kind,         std::move(message), std::nullopt,
                         std::nullopt, std::nullopt,       std::nullopt};
}

struct FakeNode {
  RuntimeValue value;
  bool undefined{false};
  bool callable{false};
  FakeFunctionBehavior behavior{FakeFunctionBehavior::ReturnConfiguredValue};
  std::optional<EngineException> callException;
  std::optional<std::uint64_t> nativeBindingId;
  std::map<std::string, std::shared_ptr<FakeNode>, std::less<>> properties;
};

class FakePort final : public JsEnginePort {
public:
  FakePort(JsEngineConfig config,
           std::shared_ptr<FakeEngineProvider::SharedData> data,
           FakeEngineOptions options)
      : state_(std::make_shared<State>()), config_(std::move(config)),
        data_(std::move(data)), options_(options) {
    state_->serviceId = gNextServiceId.fetch_add(1, std::memory_order_relaxed);
    state_->ownerThread = std::this_thread::get_id();
  }

  ~FakePort() override {
    if (state_->context &&
        state_->context->alive.load(std::memory_order_acquire)) {
      clearContext();
    }
  }

  JsEngineDescriptor describe() const noexcept override {
    return {"fake", "1", "quickapp-kit-js-engine-v1", "engine.fake"};
  }

  EngineResult<JsContextRef> createContext() noexcept override {
    try {
      record("createContext");
      if (!onOwnerThread()) {
        return failContext("wrong executor");
      }
      if (options_.failContextCreate) {
        notifyOutOfMemory();
        return failContext("injected context creation failure",
                           EngineExceptionKind::OutOfMemory);
      }
      if (state_->context &&
          state_->context->alive.load(std::memory_order_acquire)) {
        return failContext("context already exists");
      }
      auto context = std::make_shared<detail::ContextState>();
      context->serviceId = state_->serviceId;
      context->contextId = ++state_->nextContextId;
      context->generation = ++state_->generation;
      context->ownerThread = state_->ownerThread;
      state_->context = context;
      state_->global = std::make_shared<FakeNode>();
      return EngineResult<JsContextRef>::success(
          detail::RefAccess::makeContext(std::move(context)));
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failContext("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failContext("unexpected fake engine failure");
    }
  }

  EngineResult<void> destroyContext(JsContextRef &context) noexcept override {
    record("destroyContext");
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<void>::failure(std::move(*invalid));
    }
    const auto liveValues = state_->values.size();
    const auto liveBindings = state_->bindings.size();
    clearContext();
    context = JsContextRef{};
    if (liveValues != 0 || liveBindings != 0) {
      return EngineResult<void>::failure(
          error(EngineExceptionKind::Runtime,
                "context reconciled with live values or native bindings"));
    }
    return EngineResult<void>::success();
  }

  EngineResult<JsValueRef>
  evaluate(const JsContextRef &context,
           const SourceUnit &source) noexcept override {
    try {
      record("evaluate:" + source.sourceId);
      if (state_->inNativeCallback) {
        return failValue("evaluate reentry from native callback",
                         EngineExceptionKind::NativeBinding);
      }
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<JsValueRef>::failure(std::move(*invalid));
      }
      const auto plan = planFor(source.sourceId);
      if (!plan.has_value()) {
        return failValue("no fake source plan: " + source.sourceId,
                         EngineExceptionKind::Syntax);
      }
      if (plan->exception.has_value()) {
        if (plan->exception->kind == EngineExceptionKind::OutOfMemory) {
          notifyOutOfMemory();
        }
        return EngineResult<JsValueRef>::failure(*plan->exception);
      }
      auto node = std::make_shared<FakeNode>();
      node->value = plan->result;
      node->callable = plan->callable;
      node->behavior = plan->functionBehavior;
      node->callException = plan->functionException;
      for (const auto &property : plan->callableProperties) {
        auto function = std::make_shared<FakeNode>();
        function->callable = true;
        node->properties.emplace(property, std::move(function));
      }
      for (std::uint32_t i = 0; i < plan->microtasks; ++i) {
        state_->microtasks.push_back([] {});
      }
      auto evaluated = makeValue(node);
      if (!evaluated.ok()) return evaluated;
      for (const auto &invocation : plan->nativeCalls) {
        auto global = globalObject(context);
        if (!global.ok()) return global;
        auto function = getProperty(context, global.value(), invocation.globalName);
        if (!function.ok()) return function;
        std::vector<JsValueRef> arguments;
        arguments.reserve(invocation.arguments.size());
        for (const auto &argument : invocation.arguments) {
          auto value = argument.useEvaluatedFunction
                           ? retain(context, evaluated.value())
                           : fromRuntimeValue(context, argument.value);
          if (!value.ok()) return value;
          arguments.push_back(std::move(value).value());
        }
        auto result = call(context, function.value(), global.value(), arguments);
        if (!result.ok()) return result;
      }
      return evaluated;
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failValue("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failValue("unexpected fake engine failure");
    }
  }

  EngineResult<JsValueRef>
  call(const JsContextRef &context, const JsValueRef &function,
       const JsValueRef &thisValue,
       std::span<const JsValueRef> args) noexcept override {
    try {
      record("call");
      if (state_->inNativeCallback) {
        return failValue("call reentry from native callback",
                         EngineExceptionKind::NativeBinding);
      }
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<JsValueRef>::failure(std::move(*invalid));
      }
      auto functionNode = nodeFor(function);
      if (!functionNode || !functionNode->callable) {
        return failValue("value is not callable");
      }
      if (functionNode->callException.has_value()) {
        return EngineResult<JsValueRef>::failure(*functionNode->callException);
      }
      if (!nodeFor(thisValue)) {
        return failValue("invalid this value");
      }
      std::vector<std::shared_ptr<FakeNode>> argumentNodes;
      argumentNodes.reserve(args.size());
      for (const auto &arg : args) {
        auto node = nodeFor(arg);
        if (!node) {
          return failValue("invalid argument value");
        }
        argumentNodes.push_back(std::move(node));
      }
      if (functionNode->nativeBindingId.has_value()) {
        const auto binding =
            state_->bindings.find(*functionNode->nativeBindingId);
        if (binding == state_->bindings.end()) {
          return failValue("native binding is not active",
                           EngineExceptionKind::NativeBinding);
        }
        return callNative(context, thisValue, args, binding->second);
      }
      if (functionNode->behavior == FakeFunctionBehavior::EchoFirstArgument) {
        if (args.empty()) {
          return makeValue(std::make_shared<FakeNode>());
        }
        return retain(context, args.front());
      }
      if (functionNode->behavior == FakeFunctionBehavior::SumArguments) {
        double sum = 0;
        for (const auto &node : argumentNodes) {
          const auto *number = std::get_if<double>(&node->value.storage());
          if (!number) {
            return failValue("sum argument is not a number");
          }
          sum += *number;
        }
        auto result = std::make_shared<FakeNode>();
        result->value = RuntimeValue(sum);
        return makeValue(std::move(result));
      }
      if (functionNode->behavior ==
          FakeFunctionBehavior::ReturnConfiguredData) {
        auto result = std::make_shared<FakeNode>(*functionNode);
        result->callable = false;
        result->nativeBindingId.reset();
        return makeValue(std::move(result));
      }
      if (functionNode->behavior ==
          FakeFunctionBehavior::SetModuleExportsFromConfiguredValue) {
        if (argumentNodes.size() != 3) {
          return failValue("module factory arity mismatch");
        }
        auto exports = std::make_shared<FakeNode>(*functionNode);
        exports->callable = false;
        exports->nativeBindingId.reset();
        argumentNodes[1]->properties["exports"] = std::move(exports);
        return makeValue(std::make_shared<FakeNode>());
      }
      return makeValue(std::make_shared<FakeNode>(*functionNode));
    } catch (const std::exception &exception) {
      return failValue(exception.what(), EngineExceptionKind::NativeBinding);
    } catch (...) {
      return failValue("native callback threw",
                       EngineExceptionKind::NativeBinding);
    }
  }

  EngineResult<JsValueRef>
  globalObject(const JsContextRef &context) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<JsValueRef>::failure(std::move(*invalid));
    }
    return makeValue(state_->global);
  }

  EngineResult<JsValueRef>
  getProperty(const JsContextRef &context, const JsValueRef &object,
              std::string_view name) noexcept override {
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<JsValueRef>::failure(std::move(*invalid));
      }
      auto node = nodeFor(object);
      if (!node) {
        return failValue("invalid object");
      }
      if (auto found = node->properties.find(name);
          found != node->properties.end()) {
        return makeValue(found->second);
      }
      if (auto *values =
              std::get_if<RuntimeValue::Object>(&node->value.storage())) {
        if (auto found = values->find(name); found != values->end()) {
          auto child = std::make_shared<FakeNode>();
          child->value = found->second;
          return makeValue(std::move(child));
        }
      }
      auto missing = std::make_shared<FakeNode>();
      missing->undefined = true;
      return makeValue(std::move(missing));
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failValue("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failValue("property access failed");
    }
  }

  EngineResult<void> setProperty(const JsContextRef &context,
                                 JsValueRef &object, std::string_view name,
                                 const JsValueRef &value) noexcept override {
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<void>::failure(std::move(*invalid));
      }
      auto objectNode = nodeFor(object);
      auto valueNode = nodeFor(value);
      if (!objectNode || !valueNode || name.empty()) {
        return EngineResult<void>::failure(
            error(EngineExceptionKind::Runtime, "invalid property assignment"));
      }
      objectNode->properties[std::string(name)] = std::move(valueNode);
      return EngineResult<void>::success();
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return EngineResult<void>::failure(
          error(EngineExceptionKind::OutOfMemory, "out of memory"));
    } catch (...) {
      return EngineResult<void>::failure(
          error(EngineExceptionKind::Runtime, "property assignment failed"));
    }
  }

  EngineResult<bool> isCallable(const JsContextRef &context,
                                const JsValueRef &value) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<bool>::failure(std::move(*invalid));
    }
    auto node = nodeFor(value);
    if (!node) {
      return EngineResult<bool>::failure(
          error(EngineExceptionKind::Runtime, "invalid value"));
    }
    return EngineResult<bool>::success(node->callable);
  }

  EngineResult<JsValueRef>
  fromRuntimeValue(const JsContextRef &context,
                   const RuntimeValue &value) noexcept override {
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<JsValueRef>::failure(std::move(*invalid));
      }
      const ValueLimits limits{config_.limits.maxRuntimeValueDepth,
                               config_.limits.maxRuntimeValueNodes};
      if (!validateRuntimeValue(value, limits).ok()) {
        return failValue("invalid RuntimeValue");
      }
      auto node = std::make_shared<FakeNode>();
      node->value = value;
      return makeValue(std::move(node));
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failValue("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failValue("RuntimeValue conversion failed");
    }
  }

  EngineResult<RuntimeValue>
  toRuntimeValue(const JsContextRef &context, const JsValueRef &value,
                 const ValueLimits &limits) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<RuntimeValue>::failure(std::move(*invalid));
    }
    auto node = nodeFor(value);
    if (!node || node->undefined || node->callable ||
        !validateRuntimeValue(node->value, limits).ok()) {
      return EngineResult<RuntimeValue>::failure(error(
          EngineExceptionKind::Runtime, "value is not RuntimeValue data"));
    }
    return EngineResult<RuntimeValue>::success(node->value);
  }

  EngineResult<JsValueRef> retain(const JsContextRef &context,
                                  const JsValueRef &value) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<JsValueRef>::failure(std::move(*invalid));
    }
    auto node = nodeFor(value);
    if (!node) {
      return failValue("invalid value");
    }
    return makeValue(std::move(node));
  }

  EngineResult<JsValueRef> retain(const JsContextRef &context,
                                  const JsValueView &view) noexcept override {
    const auto *value = detail::RefAccess::viewedValue(view);
    if (!value) {
      return failValue("invalid borrowed value");
    }
    return retain(context, *value);
  }

  EngineResult<NativeBindingToken>
  bindNativeFunction(const JsContextRef &context,
                     const NativeFunctionSpec &spec) noexcept override {
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<NativeBindingToken>::failure(std::move(*invalid));
      }
      if (spec.globalName.empty() || !spec.invoke ||
          state_->bindingsByName.contains(spec.globalName)) {
        return EngineResult<NativeBindingToken>::failure(
            error(EngineExceptionKind::NativeBinding,
                  "invalid or duplicate native binding"));
      }
      const auto id = ++state_->nextBindingId;
      state_->bindings.emplace(id, spec);
      state_->bindingsByName.emplace(spec.globalName, id);
      auto node = std::make_shared<FakeNode>();
      node->callable = true;
      node->nativeBindingId = id;
      state_->global->properties[spec.globalName] = std::move(node);
      return EngineResult<NativeBindingToken>::success(
          detail::RefAccess::makeBindingToken(id, state_->context->contextId,
                                              spec.globalName));
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return EngineResult<NativeBindingToken>::failure(
          error(EngineExceptionKind::OutOfMemory, "out of memory"));
    } catch (...) {
      return EngineResult<NativeBindingToken>::failure(
          error(EngineExceptionKind::NativeBinding, "native binding failed"));
    }
  }

  EngineResult<void>
  unbindNativeFunction(const JsContextRef &context,
                       NativeBindingToken &token) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<void>::failure(std::move(*invalid));
    }
    const auto id = detail::RefAccess::bindingId(token);
    if (id == 0 ||
        detail::RefAccess::bindingContextId(token) !=
            state_->context->contextId ||
        !state_->bindings.erase(id)) {
      return EngineResult<void>::failure(error(
          EngineExceptionKind::NativeBinding, "native binding is not active"));
    }
    state_->bindingsByName.erase(detail::RefAccess::bindingName(token));
    state_->global->properties.erase(detail::RefAccess::bindingName(token));
    detail::RefAccess::invalidate(token);
    return EngineResult<void>::success();
  }

  EngineResult<MicrotaskDrain>
  drainMicrotasks(const JsContextRef &context,
                  std::uint32_t maxJobs) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<MicrotaskDrain>::failure(std::move(*invalid));
    }
    if (maxJobs == 0) {
      return EngineResult<MicrotaskDrain>::failure(error(
          EngineExceptionKind::Runtime, "microtask budget must be positive"));
    }
    std::uint32_t executed = 0;
    while (executed < maxJobs && !state_->microtasks.empty()) {
      auto job = std::move(state_->microtasks.front());
      state_->microtasks.pop_front();
      job();
      ++executed;
    }
    return EngineResult<MicrotaskDrain>::success(
        MicrotaskDrain{executed, !state_->microtasks.empty()});
  }

  EngineResult<void>
  requestGarbageCollection(const JsContextRef &context) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<void>::failure(std::move(*invalid));
    }
    return EngineResult<void>::success();
  }

  EngineResult<EngineMemorySnapshot>
  snapshotMemory(const JsContextRef &context) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<EngineMemorySnapshot>::failure(std::move(*invalid));
    }
    return EngineResult<EngineMemorySnapshot>::success(EngineMemorySnapshot{
        state_->values.size() * sizeof(FakeNode), state_->values.size()});
  }

private:
  struct State final : detail::ValueOwner {
    void releaseValue(std::uint64_t valueId) noexcept override {
      if (std::this_thread::get_id() == ownerThread) {
        values.erase(valueId);
      } else {
        std::lock_guard lock(deferredMutex);
        deferredReleases.push_back(valueId);
      }
    }

    std::uint64_t serviceId{0};
    std::uint64_t nextContextId{0};
    std::uint64_t generation{0};
    std::uint64_t nextValueId{0};
    std::uint64_t nextBindingId{0};
    std::thread::id ownerThread;
    std::shared_ptr<detail::ContextState> context;
    std::shared_ptr<FakeNode> global;
    std::unordered_map<std::uint64_t, std::shared_ptr<FakeNode>> values;
    std::unordered_map<std::uint64_t, NativeFunctionSpec> bindings;
    std::map<std::string, std::uint64_t, std::less<>> bindingsByName;
    std::deque<std::function<void()>> microtasks;
    std::mutex deferredMutex;
    std::vector<std::uint64_t> deferredReleases;
    bool inNativeCallback{false};
  };

  void drainDeferredReleases() noexcept {
    std::vector<std::uint64_t> releases;
    {
      std::lock_guard lock(state_->deferredMutex);
      releases.swap(state_->deferredReleases);
    }
    for (const auto id : releases) {
      state_->values.erase(id);
    }
  }

  bool onOwnerThread() const noexcept {
    return std::this_thread::get_id() == state_->ownerThread;
  }

  std::optional<EngineException>
  validateContext(const JsContextRef &context) noexcept {
    if (!onOwnerThread()) {
      return error(EngineExceptionKind::Runtime, "wrong executor");
    }
    drainDeferredReleases();
    const auto &actual = detail::RefAccess::contextState(context);
    if (!actual || actual != state_->context ||
        !actual->alive.load(std::memory_order_acquire) ||
        actual->serviceId != state_->serviceId ||
        actual->generation != state_->generation) {
      return error(EngineExceptionKind::Runtime, "wrong or destroyed context");
    }
    return std::nullopt;
  }

  std::shared_ptr<FakeNode> nodeFor(const JsValueRef &value) noexcept {
    const auto &ref = detail::RefAccess::valueState(value);
    if (!ref || ref->serviceId != state_->serviceId || !state_->context ||
        ref->contextId != state_->context->contextId ||
        ref->generation != state_->generation) {
      return {};
    }
    const auto found = state_->values.find(ref->valueId);
    return found == state_->values.end() ? std::shared_ptr<FakeNode>{}
                                         : found->second;
  }

  EngineResult<JsValueRef> makeValue(std::shared_ptr<FakeNode> node) noexcept {
    try {
      const auto id = ++state_->nextValueId;
      state_->values.emplace(id, std::move(node));
      auto ref = std::make_shared<detail::ValueState>();
      ref->serviceId = state_->serviceId;
      ref->contextId = state_->context->contextId;
      ref->generation = state_->generation;
      ref->valueId = id;
      ref->context = state_->context;
      ref->owner = state_;
      return EngineResult<JsValueRef>::success(
          detail::RefAccess::makeValue(std::move(ref)));
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failValue("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failValue("value creation failed");
    }
  }

  EngineResult<JsValueRef> callNative(const JsContextRef &context,
                                      const JsValueRef &thisValue,
                                      std::span<const JsValueRef> args,
                                      const NativeFunctionSpec &spec) noexcept {
    if (args.size() < spec.minArgs ||
        (spec.maxArgs.has_value() && args.size() > *spec.maxArgs)) {
      return failValue("native argument count mismatch",
                       EngineExceptionKind::NativeBinding);
    }
    std::vector<JsValueView> views;
    views.reserve(args.size());
    for (const auto &arg : args) {
      views.push_back(detail::RefAccess::makeView(arg));
    }
    state_->inNativeCallback = true;
    NativeFunctionResult result = NativeFunctionResult::failure(RuntimeError{
        RuntimeErrorCode::JsException, "native callback did not complete"});
    try {
      result = spec.invoke(NativeCallView{
          context, detail::RefAccess::makeView(thisValue), views});
    } catch (const std::exception &exception) {
      state_->inNativeCallback = false;
      return failValue(exception.what(), EngineExceptionKind::NativeBinding);
    } catch (...) {
      state_->inNativeCallback = false;
      return failValue("native callback threw",
                       EngineExceptionKind::NativeBinding);
    }
    state_->inNativeCallback = false;
    if (!result.ok()) {
      return failValue(std::string(runtimeErrorCodeName(result.error().code)) +
                           ": " + result.error().message,
                       EngineExceptionKind::NativeBinding);
    }
    return EngineResult<JsValueRef>::success(std::move(result).value());
  }

  std::optional<FakeSourcePlan> planFor(const std::string &sourceId) const;
  void record(std::string operation) noexcept;
  void notifyOutOfMemory() noexcept {
    if (config_.onOutOfMemory) {
      config_.onOutOfMemory();
    }
  }
  void clearContext() noexcept {
    state_->bindings.clear();
    state_->bindingsByName.clear();
    state_->microtasks.clear();
    state_->values.clear();
    state_->global.reset();
    if (state_->context) {
      state_->context->alive.store(false, std::memory_order_release);
      state_->context.reset();
    }
  }

  static EngineResult<JsContextRef> failContext(
      std::string message,
      EngineExceptionKind kind = EngineExceptionKind::Runtime) noexcept {
    return EngineResult<JsContextRef>::failure(error(kind, std::move(message)));
  }
  static EngineResult<JsValueRef>
  failValue(std::string message,
            EngineExceptionKind kind = EngineExceptionKind::Runtime) noexcept {
    return EngineResult<JsValueRef>::failure(error(kind, std::move(message)));
  }

  std::shared_ptr<State> state_;
  JsEngineConfig config_;
  std::shared_ptr<FakeEngineProvider::SharedData> data_;
  FakeEngineOptions options_;
};

} // namespace

struct FakeEngineProvider::SharedData {
  mutable std::mutex mutex;
  std::map<std::string, FakeSourcePlan, std::less<>> plans;
  std::vector<std::string> operations;
};

std::optional<FakeSourcePlan>
FakePort::planFor(const std::string &sourceId) const {
  std::lock_guard lock(data_->mutex);
  const auto found = data_->plans.find(sourceId);
  return found == data_->plans.end() ? std::optional<FakeSourcePlan>{}
                                     : found->second;
}

void FakePort::record(std::string operation) noexcept {
  try {
    std::lock_guard lock(data_->mutex);
    data_->operations.push_back(std::move(operation));
  } catch (...) {
  }
}

FakeEngineProvider::FakeEngineProvider(FakeEngineOptions options)
    : data_(std::make_shared<SharedData>()), options_(options) {}

void FakeEngineProvider::setPlan(std::string sourceId, FakeSourcePlan plan) {
  std::lock_guard lock(data_->mutex);
  data_->plans[std::move(sourceId)] = std::move(plan);
}

const std::vector<std::string> &
FakeEngineProvider::operations() const noexcept {
  return data_->operations;
}

JsEngineDescriptor FakeEngineProvider::describe() const noexcept {
  return {"fake", "1", "quickapp-kit-js-engine-v1", "engine.fake"};
}

std::unique_ptr<JsEnginePort>
FakeEngineProvider::create(const JsEngineConfig &config) noexcept {
  if (options_.failEngineCreate) {
    if (config.onOutOfMemory) {
      config.onOutOfMemory();
    }
    return {};
  }
  try {
    return std::make_unique<FakePort>(config, data_, options_);
  } catch (...) {
    if (config.onOutOfMemory) {
      config.onOutOfMemory();
    }
    return {};
  }
}

} // namespace quickapp::js::testing
