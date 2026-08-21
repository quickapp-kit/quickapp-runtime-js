#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "quickapp/js/engine/engine_types.h"

namespace quickapp::js::detail {

class ValueOwner {
public:
  virtual ~ValueOwner() = default;
  virtual void releaseValue(std::uint64_t valueId) noexcept = 0;
};

struct ContextState {
  std::uint64_t serviceId{0};
  std::uint64_t contextId{0};
  std::uint64_t generation{0};
  std::thread::id ownerThread;
  std::atomic<bool> alive{true};
};

struct ValueState {
  std::uint64_t serviceId{0};
  std::uint64_t contextId{0};
  std::uint64_t generation{0};
  std::uint64_t valueId{0};
  std::weak_ptr<ContextState> context;
  std::weak_ptr<ValueOwner> owner;

  ~ValueState();
};

class RefAccess {
public:
  static JsContextRef makeContext(std::shared_ptr<ContextState> state) {
    return JsContextRef(std::move(state));
  }

  static JsValueRef makeValue(std::shared_ptr<ValueState> state) {
    return JsValueRef(std::move(state));
  }

  static JsValueView makeView(const JsValueRef &value) {
    return JsValueView(&value);
  }

  static const std::shared_ptr<ContextState> &
  contextState(const JsContextRef &context) {
    return context.state_;
  }

  static const std::shared_ptr<ValueState> &
  valueState(const JsValueRef &value) {
    return value.state_;
  }

  static const JsValueRef *viewedValue(const JsValueView &view) {
    return view.value_;
  }

  static NativeBindingToken makeBindingToken(std::uint64_t bindingId,
                                             std::uint64_t contextId,
                                             std::string globalName) {
    NativeBindingToken token;
    token.bindingId_ = bindingId;
    token.contextId_ = contextId;
    token.globalName_ = std::move(globalName);
    return token;
  }

  static std::uint64_t bindingId(const NativeBindingToken &token) {
    return token.bindingId_;
  }

  static std::uint64_t bindingContextId(const NativeBindingToken &token) {
    return token.contextId_;
  }

  static const std::string &bindingName(const NativeBindingToken &token) {
    return token.globalName_;
  }

  static void invalidate(NativeBindingToken &token) {
    token.bindingId_ = 0;
    token.contextId_ = 0;
    token.globalName_.clear();
  }
};

} // namespace quickapp::js::detail
