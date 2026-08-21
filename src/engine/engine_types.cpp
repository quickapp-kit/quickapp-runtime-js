#include "quickapp/js/engine/engine_types.h"

#include <utility>

#include "engine/internal/ref_access.h"

namespace quickapp::js::detail {

ValueState::~ValueState() {
  if (auto locked = owner.lock()) {
    locked->releaseValue(valueId);
  }
}

} // namespace quickapp::js::detail

namespace quickapp::js {

std::string_view runtimeErrorCodeName(RuntimeErrorCode code) noexcept {
  switch (code) {
  case RuntimeErrorCode::AbiInvalidArgument:
    return "ABI_INVALID_ARGUMENT";
  case RuntimeErrorCode::ModuleAbiUnsupported:
    return "MODULE_ABI_UNSUPPORTED";
  case RuntimeErrorCode::OutOfMemory:
    return "OUT_OF_MEMORY";
  case RuntimeErrorCode::QueueOverflow:
    return "QUEUE_OVERFLOW";
  case RuntimeErrorCode::JsException:
    return "JS_EXCEPTION";
  }
  return "JS_EXCEPTION";
}

JsContextRef::JsContextRef(std::shared_ptr<detail::ContextState> state)
    : state_(std::move(state)) {}

bool JsContextRef::valid() const noexcept {
  return state_ && state_->alive.load(std::memory_order_acquire);
}

JsValueRef::JsValueRef(std::shared_ptr<detail::ValueState> state)
    : state_(std::move(state)) {}

JsValueRef::~JsValueRef() = default;
JsValueRef::JsValueRef(JsValueRef &&) noexcept = default;
JsValueRef &JsValueRef::operator=(JsValueRef &&) noexcept = default;

bool JsValueRef::valid() const noexcept {
  if (!state_) {
    return false;
  }
  const auto context = state_->context.lock();
  return context && context->alive.load(std::memory_order_acquire);
}

void JsValueRef::reset() noexcept { state_.reset(); }

bool JsValueView::valid() const noexcept {
  return value_ != nullptr && value_->valid();
}

} // namespace quickapp::js
