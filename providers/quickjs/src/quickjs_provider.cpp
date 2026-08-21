#include "quickapp/js/engine/quickjs_engine_provider.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include "quickapp_quickjs_config.h"
}

#include "engine/internal/ref_access.h"

namespace quickapp::js {
namespace {

std::atomic<std::uint64_t> gNextServiceId{1};

EngineException exception(EngineExceptionKind kind, std::string message,
                          std::optional<std::string> stack = std::nullopt) {
  return EngineException{kind,         std::move(message), std::move(stack),
                         std::nullopt, std::nullopt,       std::nullopt};
}

class QuickJsPort final : public JsEnginePort {
public:
  explicit QuickJsPort(JsEngineConfig config)
      : state_(std::make_shared<State>()), config_(std::move(config)) {
    state_->serviceId = gNextServiceId.fetch_add(1, std::memory_order_relaxed);
    state_->ownerThread = std::this_thread::get_id();
    state_->port = this;
    state_->runtime = JS_NewRuntime();
    if (state_->runtime) {
      JS_SetMemoryLimit(state_->runtime,
                        static_cast<size_t>(config_.limits.maxHeapBytes));
      JS_SetMaxStackSize(state_->runtime,
                         static_cast<size_t>(config_.limits.maxStackBytes));
      JS_SetRuntimeOpaque(state_->runtime, state_.get());
    }
  }

  ~QuickJsPort() override {
    if (state_->context) {
      clearContext();
    }
    if (state_->runtime) {
      JS_FreeRuntime(state_->runtime);
      state_->runtime = nullptr;
    }
  }

  [[nodiscard]] bool valid() const noexcept {
    return state_->runtime != nullptr;
  }

  JsEngineDescriptor describe() const noexcept override {
    return {"quickjs", QUICKAPP_QUICKJS_VERSION, "quickapp-kit-js-engine-v1",
            "engine.quickjs"};
  }

  EngineResult<JsContextRef> createContext() noexcept override {
    try {
      if (!onOwnerThread()) {
        return failContext("wrong executor");
      }
      if (!state_->runtime || state_->context) {
        return failContext("engine is unavailable or context already exists");
      }
      JS_UpdateStackTop(state_->runtime);
      state_->context = JS_NewContext(state_->runtime);
      if (!state_->context) {
        notifyOutOfMemory();
        return failContext("out of memory", EngineExceptionKind::OutOfMemory);
      }
      JS_SetContextOpaque(state_->context, state_.get());
      JSValue object = JS_NewObject(state_->context);
      JSValue array = JS_NewArray(state_->context);
      if (JS_IsException(object) || JS_IsException(array)) {
        if (!JS_IsException(object)) {
          JS_FreeValue(state_->context, object);
        }
        if (!JS_IsException(array)) {
          JS_FreeValue(state_->context, array);
        }
        auto failure = takePendingException();
        clearContext();
        return EngineResult<JsContextRef>::failure(std::move(failure));
      }
      state_->plainObjectClass = JS_GetClassID(object);
      state_->arrayClass = JS_GetClassID(array);
      state_->objectPrototype = JS_GetPrototype(state_->context, object);
      if (JS_IsException(state_->objectPrototype)) {
        JS_FreeValue(state_->context, object);
        JS_FreeValue(state_->context, array);
        auto failure = takePendingException();
        clearContext();
        return EngineResult<JsContextRef>::failure(std::move(failure));
      }
      state_->hasObjectPrototype = true;
      JS_FreeValue(state_->context, object);
      JS_FreeValue(state_->context, array);

      auto context = std::make_shared<detail::ContextState>();
      context->serviceId = state_->serviceId;
      context->contextId = ++state_->nextContextId;
      context->generation = ++state_->generation;
      context->ownerThread = state_->ownerThread;
      state_->contextRef = context;
      return EngineResult<JsContextRef>::success(
          detail::RefAccess::makeContext(std::move(context)));
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failContext("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failContext("QuickJS context creation failed");
    }
  }

  EngineResult<void> destroyContext(JsContextRef &context) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<void>::failure(std::move(*invalid));
    }
    const auto liveValues = state_->values.size();
    const auto liveBindings = state_->bindings.size();
    clearContext();
    context = JsContextRef{};
    if (liveValues != 0 || liveBindings != 0) {
      return EngineResult<void>::failure(
          exception(EngineExceptionKind::Runtime,
                    "context reconciled with live values or native bindings"));
    }
    return EngineResult<void>::success();
  }

  EngineResult<JsValueRef>
  evaluate(const JsContextRef &context,
           const SourceUnit &source) noexcept override {
    try {
      if (state_->inNativeCallback) {
        return failValue("evaluate reentry from native callback",
                         EngineExceptionKind::NativeBinding);
      }
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<JsValueRef>::failure(std::move(*invalid));
      }
      const int flags = source.mode == SourceMode::Module ? JS_EVAL_TYPE_MODULE
                                                          : JS_EVAL_TYPE_GLOBAL;
      const char *filename = source.sourceUrl.empty()
                                 ? source.sourceId.c_str()
                                 : source.sourceUrl.c_str();
      JSValue result = JS_Eval(state_->context, source.source.c_str(),
                               source.source.size(), filename, flags);
      if (JS_IsException(result)) {
        return EngineResult<JsValueRef>::failure(takePendingException());
      }
      return adoptValue(result);
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failValue("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failValue("QuickJS evaluate failed");
    }
  }

  EngineResult<JsValueRef>
  call(const JsContextRef &context, const JsValueRef &function,
       const JsValueRef &thisValue,
       std::span<const JsValueRef> args) noexcept override {
    try {
      if (state_->inNativeCallback) {
        return failValue("call reentry from native callback",
                         EngineExceptionKind::NativeBinding);
      }
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<JsValueRef>::failure(std::move(*invalid));
      }
      const auto functionRaw = rawFor(function);
      const auto thisRaw = rawFor(thisValue);
      if (!functionRaw.has_value() || !thisRaw.has_value()) {
        return failValue("invalid function or this value");
      }
      std::vector<JSValue> rawArgs;
      rawArgs.reserve(args.size());
      for (const auto &arg : args) {
        const auto raw = rawFor(arg);
        if (!raw.has_value()) {
          return failValue("invalid argument value");
        }
        rawArgs.push_back(*raw);
      }
      JSValue result =
          JS_Call(state_->context, *functionRaw, *thisRaw,
                  static_cast<int>(rawArgs.size()), rawArgs.data());
      if (JS_IsException(result)) {
        return EngineResult<JsValueRef>::failure(takePendingException());
      }
      return adoptValue(result);
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return failValue("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      return failValue("QuickJS call failed");
    }
  }

  EngineResult<JsValueRef>
  globalObject(const JsContextRef &context) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<JsValueRef>::failure(std::move(*invalid));
    }
    JSValue global = JS_GetGlobalObject(state_->context);
    if (JS_IsException(global)) {
      return EngineResult<JsValueRef>::failure(takePendingException());
    }
    return adoptValue(global);
  }

  EngineResult<JsValueRef>
  getProperty(const JsContextRef &context, const JsValueRef &object,
              std::string_view name) noexcept override {
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<JsValueRef>::failure(std::move(*invalid));
      }
      const auto raw = rawFor(object);
      if (!raw.has_value()) {
        return failValue("invalid object");
      }
      const std::string ownedName(name);
      JSValue result =
          JS_GetPropertyStr(state_->context, *raw, ownedName.c_str());
      if (JS_IsException(result)) {
        return EngineResult<JsValueRef>::failure(takePendingException());
      }
      return adoptValue(result);
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
      const auto objectRaw = rawFor(object);
      const auto valueRaw = rawFor(value);
      if (!objectRaw.has_value() || !valueRaw.has_value() || name.empty()) {
        return EngineResult<void>::failure(exception(
            EngineExceptionKind::Runtime, "invalid property assignment"));
      }
      const std::string ownedName(name);
      if (JS_SetPropertyStr(state_->context, *objectRaw, ownedName.c_str(),
                            JS_DupValue(state_->context, *valueRaw)) < 0) {
        return EngineResult<void>::failure(takePendingException());
      }
      return EngineResult<void>::success();
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return EngineResult<void>::failure(
          exception(EngineExceptionKind::OutOfMemory, "out of memory"));
    } catch (...) {
      return EngineResult<void>::failure(exception(
          EngineExceptionKind::Runtime, "property assignment failed"));
    }
  }

  EngineResult<bool> isCallable(const JsContextRef &context,
                                const JsValueRef &value) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<bool>::failure(std::move(*invalid));
    }
    const auto raw = rawFor(value);
    if (!raw.has_value()) {
      return EngineResult<bool>::failure(
          exception(EngineExceptionKind::Runtime, "invalid value"));
    }
    return EngineResult<bool>::success(JS_IsFunction(state_->context, *raw));
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
      JSValue raw = runtimeValueToJs(value);
      if (JS_IsException(raw)) {
        return EngineResult<JsValueRef>::failure(takePendingException());
      }
      return adoptValue(raw);
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
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<RuntimeValue>::failure(std::move(*invalid));
      }
      const auto raw = rawFor(value);
      if (!raw.has_value() || limits.maxDepth == 0 || limits.maxNodes == 0) {
        return EngineResult<RuntimeValue>::failure(
            exception(EngineExceptionKind::Runtime, "invalid value or limits"));
      }
      std::unordered_set<void *> activeObjects;
      std::uint32_t nodes = 0;
      auto converted = jsToRuntimeValue(*raw, limits, 0, nodes, activeObjects);
      if (!converted.ok() && JS_HasException(state_->context)) {
        static_cast<void>(takePendingException());
      }
      return converted;
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return EngineResult<RuntimeValue>::failure(
          exception(EngineExceptionKind::OutOfMemory, "out of memory"));
    } catch (...) {
      return EngineResult<RuntimeValue>::failure(exception(
          EngineExceptionKind::Runtime, "RuntimeValue conversion failed"));
    }
  }

  EngineResult<JsValueRef> retain(const JsContextRef &context,
                                  const JsValueRef &value) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<JsValueRef>::failure(std::move(*invalid));
    }
    const auto raw = rawFor(value);
    if (!raw.has_value()) {
      return failValue("invalid value");
    }
    return adoptValue(JS_DupValue(state_->context, *raw));
  }

  EngineResult<JsValueRef> retain(const JsContextRef &context,
                                  const JsValueView &value) noexcept override {
    const auto *ref = detail::RefAccess::viewedValue(value);
    if (!ref) {
      return failValue("invalid borrowed value");
    }
    return retain(context, *ref);
  }

  EngineResult<NativeBindingToken>
  bindNativeFunction(const JsContextRef &context,
                     const NativeFunctionSpec &spec) noexcept override {
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<NativeBindingToken>::failure(std::move(*invalid));
      }
      if (spec.globalName.empty() || !spec.invoke ||
          state_->bindingsByName.contains(spec.globalName) ||
          state_->nextBindingId ==
              static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return EngineResult<NativeBindingToken>::failure(
            exception(EngineExceptionKind::NativeBinding,
                      "invalid or duplicate native binding"));
      }
      const auto id = ++state_->nextBindingId;
      state_->bindings.emplace(id, spec);
      state_->bindingsByName.emplace(spec.globalName, id);
      JSValue function = JS_NewCFunctionMagic(
          state_->context, &QuickJsPort::nativeTrampoline,
          spec.globalName.c_str(), static_cast<int>(spec.minArgs),
          JS_CFUNC_generic_magic, static_cast<int>(id));
      if (JS_IsException(function)) {
        state_->bindings.erase(id);
        state_->bindingsByName.erase(spec.globalName);
        return EngineResult<NativeBindingToken>::failure(
            takePendingException());
      }
      JSValue global = JS_GetGlobalObject(state_->context);
      if (JS_IsException(global) ||
          JS_SetPropertyStr(state_->context, global, spec.globalName.c_str(),
                            function) < 0) {
        if (!JS_IsException(global)) {
          JS_FreeValue(state_->context, global);
        } else {
          JS_FreeValue(state_->context, function);
        }
        state_->bindings.erase(id);
        state_->bindingsByName.erase(spec.globalName);
        return EngineResult<NativeBindingToken>::failure(
            takePendingException());
      }
      JS_FreeValue(state_->context, global);
      return EngineResult<NativeBindingToken>::success(
          detail::RefAccess::makeBindingToken(id, state_->contextRef->contextId,
                                              spec.globalName));
    } catch (const std::bad_alloc &) {
      notifyOutOfMemory();
      return EngineResult<NativeBindingToken>::failure(
          exception(EngineExceptionKind::OutOfMemory, "out of memory"));
    } catch (...) {
      return EngineResult<NativeBindingToken>::failure(exception(
          EngineExceptionKind::NativeBinding, "native binding failed"));
    }
  }

  EngineResult<void>
  unbindNativeFunction(const JsContextRef &context,
                       NativeBindingToken &token) noexcept override {
    try {
      if (auto invalid = validateContext(context); invalid.has_value()) {
        return EngineResult<void>::failure(std::move(*invalid));
      }
      const auto id = detail::RefAccess::bindingId(token);
      if (id == 0 ||
          detail::RefAccess::bindingContextId(token) !=
              state_->contextRef->contextId ||
          !state_->bindings.contains(id)) {
        return EngineResult<void>::failure(
            exception(EngineExceptionKind::NativeBinding,
                      "native binding is not active"));
      }
      JSValue global = JS_GetGlobalObject(state_->context);
      if (JS_IsException(global) ||
          JS_SetPropertyStr(state_->context, global,
                            detail::RefAccess::bindingName(token).c_str(),
                            JS_UNDEFINED) < 0) {
        if (!JS_IsException(global)) {
          JS_FreeValue(state_->context, global);
        }
        return EngineResult<void>::failure(takePendingException());
      }
      JS_FreeValue(state_->context, global);
      state_->bindings.erase(id);
      state_->bindingsByName.erase(detail::RefAccess::bindingName(token));
      detail::RefAccess::invalidate(token);
      return EngineResult<void>::success();
    } catch (...) {
      return EngineResult<void>::failure(exception(
          EngineExceptionKind::NativeBinding, "native unbind failed"));
    }
  }

  EngineResult<MicrotaskDrain>
  drainMicrotasks(const JsContextRef &context,
                  std::uint32_t maxJobs) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<MicrotaskDrain>::failure(std::move(*invalid));
    }
    if (maxJobs == 0) {
      return EngineResult<MicrotaskDrain>::failure(exception(
          EngineExceptionKind::Runtime, "microtask budget must be positive"));
    }
    std::uint32_t executed = 0;
    while (executed < maxJobs && JS_IsJobPending(state_->runtime)) {
      JSContext *jobContext = nullptr;
      const int result = JS_ExecutePendingJob(state_->runtime, &jobContext);
      if (result < 0) {
        if (jobContext && jobContext != state_->context) {
          return EngineResult<MicrotaskDrain>::failure(
              exception(EngineExceptionKind::Runtime,
                        "pending job used an unknown context"));
        }
        return EngineResult<MicrotaskDrain>::failure(takePendingException());
      }
      if (result == 0) {
        break;
      }
      ++executed;
    }
    return EngineResult<MicrotaskDrain>::success(MicrotaskDrain{
        executed, static_cast<bool>(JS_IsJobPending(state_->runtime))});
  }

  EngineResult<void>
  requestGarbageCollection(const JsContextRef &context) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<void>::failure(std::move(*invalid));
    }
    JS_RunGC(state_->runtime);
    return EngineResult<void>::success();
  }

  EngineResult<EngineMemorySnapshot>
  snapshotMemory(const JsContextRef &context) noexcept override {
    if (auto invalid = validateContext(context); invalid.has_value()) {
      return EngineResult<EngineMemorySnapshot>::failure(std::move(*invalid));
    }
    JSMemoryUsage usage{};
    JS_ComputeMemoryUsage(state_->runtime, &usage);
    return EngineResult<EngineMemorySnapshot>::success(
        EngineMemorySnapshot{static_cast<std::uint64_t>(std::max<std::int64_t>(
                                 0, usage.memory_used_size)),
                             static_cast<std::uint64_t>(
                                 std::max<std::int64_t>(0, usage.obj_count))});
  }

private:
  struct State final : detail::ValueOwner {
    void releaseValue(std::uint64_t valueId) noexcept override {
      if (std::this_thread::get_id() == ownerThread) {
        const auto found = values.find(valueId);
        if (found != values.end()) {
          if (context) {
            JS_FreeValue(context, found->second);
          }
          values.erase(found);
        }
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
    JSRuntime *runtime{nullptr};
    JSContext *context{nullptr};
    JSClassID plainObjectClass{JS_INVALID_CLASS_ID};
    JSClassID arrayClass{JS_INVALID_CLASS_ID};
    JSValue objectPrototype{};
    bool hasObjectPrototype{false};
    std::shared_ptr<detail::ContextState> contextRef;
    std::unordered_map<std::uint64_t, JSValue> values;
    std::unordered_map<std::uint64_t, NativeFunctionSpec> bindings;
    std::map<std::string, std::uint64_t, std::less<>> bindingsByName;
    std::mutex deferredMutex;
    std::vector<std::uint64_t> deferredReleases;
    bool inNativeCallback{false};
    QuickJsPort *port{nullptr};
  };

  bool onOwnerThread() const noexcept {
    return std::this_thread::get_id() == state_->ownerThread;
  }

  void drainDeferredReleases() noexcept {
    std::vector<std::uint64_t> releases;
    {
      std::lock_guard lock(state_->deferredMutex);
      releases.swap(state_->deferredReleases);
    }
    for (const auto id : releases) {
      state_->releaseValue(id);
    }
  }

  std::optional<EngineException>
  validateContext(const JsContextRef &context) noexcept {
    if (!onOwnerThread()) {
      return exception(EngineExceptionKind::Runtime, "wrong executor");
    }
    drainDeferredReleases();
    const auto &actual = detail::RefAccess::contextState(context);
    if (!actual || actual != state_->contextRef || !state_->context ||
        !actual->alive.load(std::memory_order_acquire) ||
        actual->serviceId != state_->serviceId ||
        actual->generation != state_->generation) {
      return exception(EngineExceptionKind::Runtime,
                       "wrong or destroyed context");
    }
    return std::nullopt;
  }

  std::optional<JSValue> rawFor(const JsValueRef &value) const noexcept {
    const auto &ref = detail::RefAccess::valueState(value);
    if (!ref || !state_->contextRef || ref->serviceId != state_->serviceId ||
        ref->contextId != state_->contextRef->contextId ||
        ref->generation != state_->generation) {
      return std::nullopt;
    }
    const auto found = state_->values.find(ref->valueId);
    return found == state_->values.end() ? std::optional<JSValue>{}
                                         : found->second;
  }

  EngineResult<JsValueRef> adoptValue(JSValue raw) noexcept {
    try {
      const auto id = ++state_->nextValueId;
      state_->values.emplace(id, raw);
      auto ref = std::make_shared<detail::ValueState>();
      ref->serviceId = state_->serviceId;
      ref->contextId = state_->contextRef->contextId;
      ref->generation = state_->generation;
      ref->valueId = id;
      ref->context = state_->contextRef;
      ref->owner = state_;
      return EngineResult<JsValueRef>::success(
          detail::RefAccess::makeValue(std::move(ref)));
    } catch (const std::bad_alloc &) {
      JS_FreeValue(state_->context, raw);
      notifyOutOfMemory();
      return failValue("out of memory", EngineExceptionKind::OutOfMemory);
    } catch (...) {
      JS_FreeValue(state_->context, raw);
      return failValue("value registration failed");
    }
  }

  JSValue runtimeValueToJs(const RuntimeValue &value) noexcept {
    if (std::holds_alternative<std::nullptr_t>(value.storage())) {
      return JS_NULL;
    }
    if (const auto *boolean = std::get_if<bool>(&value.storage())) {
      return JS_NewBool(state_->context, *boolean);
    }
    if (const auto *number = std::get_if<double>(&value.storage())) {
      return JS_NewFloat64(state_->context, *number);
    }
    if (const auto *string = std::get_if<std::string>(&value.storage())) {
      return JS_NewStringLen(state_->context, string->data(), string->size());
    }
    if (const auto *array =
            std::get_if<RuntimeValue::Array>(&value.storage())) {
      JSValue result = JS_NewArray(state_->context);
      if (JS_IsException(result)) {
        return result;
      }
      for (std::uint32_t index = 0; index < array->size(); ++index) {
        JSValue child = runtimeValueToJs((*array)[index]);
        if (JS_IsException(child) ||
            JS_SetPropertyUint32(state_->context, result, index, child) < 0) {
          JS_FreeValue(state_->context, result);
          return JS_EXCEPTION;
        }
      }
      return result;
    }
    JSValue result = JS_NewObject(state_->context);
    if (JS_IsException(result)) {
      return result;
    }
    const auto &object = std::get<RuntimeValue::Object>(value.storage());
    for (const auto &[key, childValue] : object) {
      JSValue child = runtimeValueToJs(childValue);
      if (JS_IsException(child) ||
          JS_SetPropertyStr(state_->context, result, key.c_str(), child) < 0) {
        JS_FreeValue(state_->context, result);
        return JS_EXCEPTION;
      }
    }
    return result;
  }

  EngineResult<RuntimeValue>
  jsToRuntimeValue(JSValueConst value, const ValueLimits &limits,
                   std::uint32_t depth, std::uint32_t &nodes,
                   std::unordered_set<void *> &activeObjects) noexcept {
    if (depth > limits.maxDepth || nodes >= limits.maxNodes) {
      return EngineResult<RuntimeValue>::failure(exception(
          EngineExceptionKind::Runtime, "RuntimeValue limits exceeded"));
    }
    ++nodes;
    if (JS_IsNull(value)) {
      return EngineResult<RuntimeValue>::success(RuntimeValue(nullptr));
    }
    if (JS_IsBool(value)) {
      return EngineResult<RuntimeValue>::success(
          RuntimeValue(static_cast<bool>(JS_ToBool(state_->context, value))));
    }
    if (JS_IsNumber(value)) {
      double number = 0;
      if (JS_ToFloat64(state_->context, &number, value) < 0 ||
          !isSafeRuntimeNumber(number)) {
        return EngineResult<RuntimeValue>::failure(exception(
            EngineExceptionKind::Runtime, "invalid RuntimeValue number"));
      }
      return EngineResult<RuntimeValue>::success(RuntimeValue(number));
    }
    if (JS_IsString(value)) {
      size_t length = 0;
      const char *chars = JS_ToCStringLen(state_->context, &length, value);
      if (!chars) {
        return EngineResult<RuntimeValue>::failure(takePendingException());
      }
      std::string string(chars, length);
      JS_FreeCString(state_->context, chars);
      return EngineResult<RuntimeValue>::success(
          RuntimeValue(std::move(string)));
    }
    if (!JS_IsObject(value) || JS_IsFunction(state_->context, value) ||
        JS_IsBigInt(state_->context, value) || JS_IsSymbol(value)) {
      return EngineResult<RuntimeValue>::failure(exception(
          EngineExceptionKind::Runtime, "value is not RuntimeValue data"));
    }
    void *identity = JS_VALUE_GET_PTR(value);
    if (!activeObjects.insert(identity).second) {
      return EngineResult<RuntimeValue>::failure(
          exception(EngineExceptionKind::Runtime, "cyclic RuntimeValue"));
    }
    struct ActiveGuard {
      std::unordered_set<void *> &set;
      void *identity;
      ~ActiveGuard() { set.erase(identity); }
    } guard{activeObjects, identity};

    const auto classId = JS_GetClassID(value);
    if (classId == state_->arrayClass) {
      JSValue lengthValue = JS_GetPropertyStr(state_->context, value, "length");
      std::uint32_t length = 0;
      if (JS_IsException(lengthValue) ||
          JS_ToUint32(state_->context, &length, lengthValue) < 0) {
        JS_FreeValue(state_->context, lengthValue);
        return EngineResult<RuntimeValue>::failure(takePendingException());
      }
      JS_FreeValue(state_->context, lengthValue);
      RuntimeValue::Array array;
      array.reserve(length);
      for (std::uint32_t index = 0; index < length; ++index) {
        JSAtom atom = JS_NewAtomUInt32(state_->context, index);
        JSPropertyDescriptor descriptor{};
        const int found =
            JS_GetOwnProperty(state_->context, &descriptor, value, atom);
        JS_FreeAtom(state_->context, atom);
        if (found <= 0 || !JS_IsUndefined(descriptor.getter) ||
            !JS_IsUndefined(descriptor.setter)) {
          if (found > 0) {
            freeDescriptor(descriptor);
          }
          return EngineResult<RuntimeValue>::failure(exception(
              EngineExceptionKind::Runtime, "array contains hole or accessor"));
        }
        auto child = jsToRuntimeValue(descriptor.value, limits, depth + 1,
                                      nodes, activeObjects);
        freeDescriptor(descriptor);
        if (!child.ok()) {
          return child;
        }
        array.push_back(std::move(child).value());
      }
      return EngineResult<RuntimeValue>::success(
          RuntimeValue(std::move(array)));
    }
    if (classId != state_->plainObjectClass) {
      return EngineResult<RuntimeValue>::failure(exception(
          EngineExceptionKind::Runtime, "object is not a plain object"));
    }
    JSValue prototype = JS_GetPrototype(state_->context, value);
    if (JS_IsException(prototype)) {
      return EngineResult<RuntimeValue>::failure(takePendingException());
    }
    const bool plainPrototype =
        JS_IsNull(prototype) ||
        (state_->hasObjectPrototype &&
         JS_StrictEq(state_->context, prototype, state_->objectPrototype));
    JS_FreeValue(state_->context, prototype);
    if (!plainPrototype) {
      return EngineResult<RuntimeValue>::failure(exception(
          EngineExceptionKind::Runtime, "object has a non-plain prototype"));
    }

    JSPropertyEnum *properties = nullptr;
    std::uint32_t propertyCount = 0;
    if (JS_GetOwnPropertyNames(state_->context, &properties, &propertyCount,
                               value,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
      return EngineResult<RuntimeValue>::failure(takePendingException());
    }
    RuntimeValue::Object object;
    for (std::uint32_t index = 0; index < propertyCount; ++index) {
      size_t keyLength = 0;
      const char *chars = JS_AtomToCStringLen(state_->context, &keyLength,
                                              properties[index].atom);
      if (!chars) {
        JS_FreePropertyEnum(state_->context, properties, propertyCount);
        return EngineResult<RuntimeValue>::failure(takePendingException());
      }
      std::string key(chars, keyLength);
      JS_FreeCString(state_->context, chars);
      JSPropertyDescriptor descriptor{};
      const int found = JS_GetOwnProperty(state_->context, &descriptor, value,
                                          properties[index].atom);
      if (found <= 0 || !JS_IsUndefined(descriptor.getter) ||
          !JS_IsUndefined(descriptor.setter)) {
        if (found > 0) {
          freeDescriptor(descriptor);
        }
        JS_FreePropertyEnum(state_->context, properties, propertyCount);
        return EngineResult<RuntimeValue>::failure(exception(
            EngineExceptionKind::Runtime, "object contains accessor"));
      }
      auto child = jsToRuntimeValue(descriptor.value, limits, depth + 1, nodes,
                                    activeObjects);
      freeDescriptor(descriptor);
      if (!child.ok()) {
        JS_FreePropertyEnum(state_->context, properties, propertyCount);
        return child;
      }
      object.emplace(std::move(key), std::move(child).value());
    }
    JS_FreePropertyEnum(state_->context, properties, propertyCount);
    return EngineResult<RuntimeValue>::success(RuntimeValue(std::move(object)));
  }

  void freeDescriptor(JSPropertyDescriptor &descriptor) noexcept {
    JS_FreeValue(state_->context, descriptor.value);
    JS_FreeValue(state_->context, descriptor.getter);
    JS_FreeValue(state_->context, descriptor.setter);
  }

  EngineException takePendingException() noexcept {
    JSValue raw = JS_GetException(state_->context);
    EngineExceptionKind kind = EngineExceptionKind::Runtime;
    std::string message = "JavaScript exception";
    std::optional<std::string> stack;

    JSValue nameValue = JS_GetPropertyStr(state_->context, raw, "name");
    const std::string name = valueToDiagnosticString(nameValue);
    JS_FreeValue(state_->context, nameValue);
    JSValue messageValue = JS_GetPropertyStr(state_->context, raw, "message");
    const std::string extracted = valueToDiagnosticString(messageValue);
    JS_FreeValue(state_->context, messageValue);
    if (!extracted.empty()) {
      message = extracted;
    }
    JSValue stackValue = JS_GetPropertyStr(state_->context, raw, "stack");
    const std::string extractedStack = valueToDiagnosticString(stackValue);
    JS_FreeValue(state_->context, stackValue);
    if (!extractedStack.empty()) {
      stack = extractedStack;
    }
    JSValue nativeMarker =
        JS_GetPropertyStr(state_->context, raw, "__quickappNativeBinding");
    const bool nativeFailure = !JS_IsException(nativeMarker) &&
                               JS_ToBool(state_->context, nativeMarker) == 1;
    JS_FreeValue(state_->context, nativeMarker);
    JS_FreeValue(state_->context, raw);

    if (nativeFailure) {
      kind = EngineExceptionKind::NativeBinding;
    } else if (name == "SyntaxError") {
      kind = EngineExceptionKind::Syntax;
    } else if (name == "InternalError" &&
               message.find("out of memory") != std::string::npos) {
      kind = EngineExceptionKind::OutOfMemory;
      notifyOutOfMemory();
    }
    return exception(kind, std::move(message), std::move(stack));
  }

  std::string valueToDiagnosticString(JSValueConst value) noexcept {
    if (JS_IsException(value) || JS_IsUndefined(value) || JS_IsNull(value)) {
      return {};
    }
    size_t length = 0;
    const char *chars = JS_ToCStringLen(state_->context, &length, value);
    if (!chars) {
      if (JS_HasException(state_->context)) {
        JSValue ignored = JS_GetException(state_->context);
        JS_FreeValue(state_->context, ignored);
      }
      return {};
    }
    try {
      std::string result(chars, length);
      JS_FreeCString(state_->context, chars);
      return result;
    } catch (...) {
      JS_FreeCString(state_->context, chars);
      return {};
    }
  }

  static JSValue nativeTrampoline(JSContext *context, JSValueConst thisValue,
                                  int argc, JSValueConst *argv,
                                  int magic) noexcept {
    auto *state = static_cast<State *>(JS_GetContextOpaque(context));
    if (!state || !state->contextRef ||
        std::this_thread::get_id() != state->ownerThread) {
      return JS_ThrowInternalError(context,
                                   "native binding owner is unavailable");
    }
    const auto found = state->bindings.find(static_cast<std::uint64_t>(magic));
    if (found == state->bindings.end()) {
      return JS_ThrowTypeError(context, "native binding is not active");
    }
    const auto &spec = found->second;
    if (argc < static_cast<int>(spec.minArgs) ||
        (spec.maxArgs.has_value() && argc > static_cast<int>(*spec.maxArgs))) {
      return JS_ThrowTypeError(context, "native argument count mismatch");
    }
    auto *port = state->port;
    if (!port) {
      return JS_ThrowInternalError(context,
                                   "native binding port is unavailable");
    }
    return port->invokeNative(spec, thisValue, argc, argv);
  }

  JSValue invokeNative(const NativeFunctionSpec &spec, JSValueConst thisValue,
                       int argc, JSValueConst *argv) noexcept {
    try {
      std::vector<JsValueRef> owned;
      std::vector<JsValueView> views;
      owned.reserve(static_cast<std::size_t>(argc) + 1);
      views.reserve(static_cast<std::size_t>(argc));
      auto thisRef = adoptValue(JS_DupValue(state_->context, thisValue));
      if (!thisRef.ok()) {
        return JS_ThrowOutOfMemory(state_->context);
      }
      owned.push_back(std::move(thisRef).value());
      for (int index = 0; index < argc; ++index) {
        auto arg = adoptValue(JS_DupValue(state_->context, argv[index]));
        if (!arg.ok()) {
          return JS_ThrowOutOfMemory(state_->context);
        }
        owned.push_back(std::move(arg).value());
        views.push_back(detail::RefAccess::makeView(owned.back()));
      }
      state_->inNativeCallback = true;
      NativeFunctionResult result = NativeFunctionResult::failure(RuntimeError{
          RuntimeErrorCode::JsException, "native callback did not complete"});
      try {
        result = spec.invoke(
            NativeCallView{detail::RefAccess::makeContext(state_->contextRef),
                           detail::RefAccess::makeView(owned.front()), views});
      } catch (const std::exception &callbackException) {
        state_->inNativeCallback = false;
        return throwNativeFailure(callbackException.what(), std::nullopt);
      } catch (...) {
        state_->inNativeCallback = false;
        return throwNativeFailure("native callback threw", std::nullopt);
      }
      state_->inNativeCallback = false;
      if (!result.ok()) {
        return throwRuntimeError(result.error());
      }
      const auto raw = rawFor(result.value());
      if (!raw.has_value()) {
        return JS_ThrowTypeError(state_->context,
                                 "native callback returned invalid value");
      }
      return JS_DupValue(state_->context, *raw);
    } catch (const std::bad_alloc &) {
      state_->inNativeCallback = false;
      notifyOutOfMemory();
      return JS_ThrowOutOfMemory(state_->context);
    } catch (...) {
      state_->inNativeCallback = false;
      return throwNativeFailure("native adapter failed", std::nullopt);
    }
  }

  JSValue throwRuntimeError(const RuntimeError &errorValue) noexcept {
    return throwNativeFailure(errorValue.message, errorValue.code);
  }

  JSValue
  throwNativeFailure(std::string_view messageText,
                     std::optional<RuntimeErrorCode> runtimeCode) noexcept {
    JSValue errorObject = JS_NewError(state_->context);
    if (JS_IsException(errorObject)) {
      return errorObject;
    }
    JSValue message = JS_NewStringLen(state_->context, messageText.data(),
                                      messageText.size());
    if (JS_IsException(message)) {
      JS_FreeValue(state_->context, errorObject);
      return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(state_->context, errorObject, "message", message) <
        0) {
      JS_FreeValue(state_->context, errorObject);
      return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(state_->context, errorObject,
                          "__quickappNativeBinding",
                          JS_NewBool(state_->context, true)) < 0) {
      JS_FreeValue(state_->context, errorObject);
      return JS_EXCEPTION;
    }
    if (runtimeCode.has_value()) {
      const auto codeName = runtimeErrorCodeName(*runtimeCode);
      JSValue code =
          JS_NewStringLen(state_->context, codeName.data(), codeName.size());
      if (JS_IsException(code) ||
          JS_SetPropertyStr(state_->context, errorObject, "code", code) < 0) {
        JS_FreeValue(state_->context, errorObject);
        return JS_EXCEPTION;
      }
    }
    return JS_Throw(state_->context, errorObject);
  }

  void clearContext() noexcept {
    if (!state_->context) {
      return;
    }
    drainDeferredReleases();
    state_->bindings.clear();
    state_->bindingsByName.clear();
    for (auto &[id, value] : state_->values) {
      static_cast<void>(id);
      JS_FreeValue(state_->context, value);
    }
    state_->values.clear();
    if (state_->hasObjectPrototype) {
      JS_FreeValue(state_->context, state_->objectPrototype);
      state_->hasObjectPrototype = false;
    }
    if (state_->contextRef) {
      state_->contextRef->alive.store(false, std::memory_order_release);
      state_->contextRef.reset();
    }
    JS_SetContextOpaque(state_->context, nullptr);
    JS_FreeContext(state_->context);
    state_->context = nullptr;
  }

  void notifyOutOfMemory() noexcept {
    if (config_.onOutOfMemory) {
      config_.onOutOfMemory();
    }
  }

  static EngineResult<JsContextRef> failContext(
      std::string message,
      EngineExceptionKind kind = EngineExceptionKind::Runtime) noexcept {
    return EngineResult<JsContextRef>::failure(
        exception(kind, std::move(message)));
  }
  static EngineResult<JsValueRef>
  failValue(std::string message,
            EngineExceptionKind kind = EngineExceptionKind::Runtime) noexcept {
    return EngineResult<JsValueRef>::failure(
        exception(kind, std::move(message)));
  }

  std::shared_ptr<State> state_;
  JsEngineConfig config_;
};

} // namespace

JsEngineDescriptor QuickJsEngineProvider::describe() const noexcept {
  return {"quickjs", QUICKAPP_QUICKJS_VERSION, "quickapp-kit-js-engine-v1",
          "engine.quickjs"};
}

std::unique_ptr<JsEnginePort>
QuickJsEngineProvider::create(const JsEngineConfig &config) noexcept {
  try {
    auto port = std::make_unique<QuickJsPort>(config);
    if (!port->valid()) {
      if (config.onOutOfMemory) {
        config.onOutOfMemory();
      }
      return {};
    }
    return port;
  } catch (...) {
    if (config.onOutOfMemory) {
      config.onOutOfMemory();
    }
    return {};
  }
}

} // namespace quickapp::js
