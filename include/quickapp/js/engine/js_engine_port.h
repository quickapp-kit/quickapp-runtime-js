#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "quickapp/js/engine/engine_types.h"

namespace quickapp::js {

class JsEnginePort {
public:
  virtual ~JsEnginePort() = default;

  [[nodiscard]] virtual JsEngineDescriptor describe() const noexcept = 0;
  [[nodiscard]] virtual EngineResult<JsContextRef> createContext() noexcept = 0;
  [[nodiscard]] virtual EngineResult<void>
  destroyContext(JsContextRef &context) noexcept = 0;

  [[nodiscard]] virtual EngineResult<JsValueRef>
  evaluate(const JsContextRef &context, const SourceUnit &source) noexcept = 0;
  [[nodiscard]] virtual EngineResult<JsValueRef>
  call(const JsContextRef &context, const JsValueRef &function,
       const JsValueRef &thisValue,
       std::span<const JsValueRef> args) noexcept = 0;

  [[nodiscard]] virtual EngineResult<JsValueRef>
  globalObject(const JsContextRef &context) noexcept = 0;
  [[nodiscard]] virtual EngineResult<JsValueRef>
  getProperty(const JsContextRef &context, const JsValueRef &object,
              std::string_view name) noexcept = 0;
  [[nodiscard]] virtual EngineResult<void>
  setProperty(const JsContextRef &context, JsValueRef &object,
              std::string_view name, const JsValueRef &value) noexcept = 0;
  [[nodiscard]] virtual EngineResult<bool>
  isCallable(const JsContextRef &context, const JsValueRef &value) noexcept = 0;

  [[nodiscard]] virtual EngineResult<JsValueRef>
  fromRuntimeValue(const JsContextRef &context,
                   const RuntimeValue &value) noexcept = 0;
  [[nodiscard]] virtual EngineResult<RuntimeValue>
  toRuntimeValue(const JsContextRef &context, const JsValueRef &value,
                 const ValueLimits &limits) noexcept = 0;
  [[nodiscard]] virtual EngineResult<JsValueRef>
  retain(const JsContextRef &context, const JsValueRef &value) noexcept = 0;
  [[nodiscard]] virtual EngineResult<JsValueRef>
  retain(const JsContextRef &context, const JsValueView &value) noexcept = 0;

  [[nodiscard]] virtual EngineResult<NativeBindingToken>
  bindNativeFunction(const JsContextRef &context,
                     const NativeFunctionSpec &spec) noexcept = 0;
  [[nodiscard]] virtual EngineResult<void>
  unbindNativeFunction(const JsContextRef &context,
                       NativeBindingToken &token) noexcept = 0;

  [[nodiscard]] virtual EngineResult<MicrotaskDrain>
  drainMicrotasks(const JsContextRef &context,
                  std::uint32_t maxJobs) noexcept = 0;
  [[nodiscard]] virtual EngineResult<void>
  requestGarbageCollection(const JsContextRef &context) noexcept = 0;
  [[nodiscard]] virtual EngineResult<EngineMemorySnapshot>
  snapshotMemory(const JsContextRef &context) noexcept = 0;
};

class JsEngineProvider {
public:
  virtual ~JsEngineProvider() = default;
  [[nodiscard]] virtual JsEngineDescriptor describe() const noexcept = 0;
  [[nodiscard]] virtual std::unique_ptr<JsEnginePort>
  create(const JsEngineConfig &config) noexcept = 0;
};

} // namespace quickapp::js
