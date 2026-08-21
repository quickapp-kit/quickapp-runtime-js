#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "quickapp/js/engine/result.h"
#include "quickapp/js/engine/runtime_value.h"

namespace quickapp::js {

namespace detail {
struct ContextState;
struct ValueState;
class RefAccess;
} // namespace detail

struct JsEngineDescriptor {
  std::string engineId;
  std::string engineVersion;
  std::string engineAbi;
  std::string moduleId;

  friend bool operator==(const JsEngineDescriptor &,
                         const JsEngineDescriptor &) = default;
};

struct JsEngineLimits {
  std::uint64_t maxHeapBytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maxStackBytes{1ULL * 1024ULL * 1024ULL};
  std::uint32_t maxPendingTasks{1024};
  std::uint32_t maxMicrotasksPerTurn{256};
  std::uint32_t maxRuntimeValueDepth{64};
  std::uint32_t maxRuntimeValueNodes{10000};
};

struct JsEngineConfig {
  JsEngineDescriptor expectedEngine;
  JsEngineLimits limits;
  std::function<void()> onOutOfMemory;
};

enum class SourceMode { Script, Module };

struct SourceUnit {
  std::string sourceId;
  std::string sourceUrl;
  std::string source;
  SourceMode mode{SourceMode::Script};
};

enum class EngineExceptionKind {
  Syntax,
  Runtime,
  Terminated,
  OutOfMemory,
  NativeBinding,
};

struct EngineException {
  EngineExceptionKind kind{EngineExceptionKind::Runtime};
  std::string message;
  std::optional<std::string> stack;
  std::optional<std::string> sourceUrl;
  std::optional<std::uint32_t> line;
  std::optional<std::uint32_t> column;
};

template <typename T> using EngineResult = Result<T, EngineException>;

enum class RuntimeErrorCode {
  AbiInvalidArgument,
  ModuleAbiUnsupported,
  OutOfMemory,
  QueueOverflow,
  JsException,
};

struct RuntimeError {
  RuntimeErrorCode code{RuntimeErrorCode::JsException};
  std::string message;
};

[[nodiscard]] std::string_view
runtimeErrorCodeName(RuntimeErrorCode code) noexcept;

class JsContextRef {
public:
  JsContextRef() = default;
  [[nodiscard]] bool valid() const noexcept;

private:
  explicit JsContextRef(std::shared_ptr<detail::ContextState> state);
  std::shared_ptr<detail::ContextState> state_;
  friend class detail::RefAccess;
};

class JsValueRef {
public:
  JsValueRef() = default;
  ~JsValueRef();
  JsValueRef(JsValueRef &&) noexcept;
  JsValueRef &operator=(JsValueRef &&) noexcept;
  JsValueRef(const JsValueRef &) = delete;
  JsValueRef &operator=(const JsValueRef &) = delete;

  [[nodiscard]] bool valid() const noexcept;
  void reset() noexcept;

private:
  explicit JsValueRef(std::shared_ptr<detail::ValueState> state);
  std::shared_ptr<detail::ValueState> state_;
  friend class detail::RefAccess;
};

class JsValueView {
public:
  JsValueView() = default;
  [[nodiscard]] bool valid() const noexcept;

private:
  explicit JsValueView(const JsValueRef *value) : value_(value) {}
  const JsValueRef *value_{nullptr};
  friend class detail::RefAccess;
};

struct MicrotaskDrain {
  std::uint32_t jobsExecuted{0};
  bool pending{false};
};

struct EngineMemorySnapshot {
  std::uint64_t allocatedBytes{0};
  std::uint64_t objectCount{0};
};

class NativeBindingToken {
public:
  NativeBindingToken() = default;
  NativeBindingToken(NativeBindingToken &&) noexcept = default;
  NativeBindingToken &operator=(NativeBindingToken &&) noexcept = default;
  NativeBindingToken(const NativeBindingToken &) = delete;
  NativeBindingToken &operator=(const NativeBindingToken &) = delete;
  [[nodiscard]] bool valid() const noexcept { return bindingId_ != 0; }

private:
  std::uint64_t bindingId_{0};
  std::uint64_t contextId_{0};
  std::string globalName_;
  friend class detail::RefAccess;
};

struct NativeCallView {
  const JsContextRef &context;
  JsValueView thisValue;
  std::span<const JsValueView> args;
};

using NativeFunctionResult = Result<JsValueRef, RuntimeError>;
using NativeFunction =
    std::function<NativeFunctionResult(const NativeCallView &)>;

struct NativeFunctionSpec {
  std::string globalName;
  std::uint32_t minArgs{0};
  std::optional<std::uint32_t> maxArgs;
  NativeFunction invoke;
};

} // namespace quickapp::js
