#include "quickapp/js/engine/js_executor_backend.h"

#include <utility>

namespace quickapp::js {

JsExecutorBackend::JsExecutorBackend(std::size_t maxPendingTasks,
                                     JsExecutor::OverflowObserver observer)
    : executor_(maxPendingTasks, std::move(observer)) {}

JsExecutorBackend::~JsExecutorBackend() = default;

bool JsExecutorBackend::start(ExecutorMode mode) {
  mode_ = mode;
  return executor_.start(mode);
}

PostResult JsExecutorBackend::post(ExecutorTask task) {
  return executor_.post(std::move(task));
}

PostResult JsExecutorBackend::postUniqueMicrotaskContinuation(ExecutorTask task) {
  return executor_.postUniqueMicrotaskContinuation(std::move(task));
}

bool JsExecutorBackend::beginStop(std::function<void()> teardownBarrier,
                                  std::function<void()> stoppedCallback) {
  return executor_.beginStop(std::move(teardownBarrier),
                             std::move(stoppedCallback));
}

bool JsExecutorBackend::stopAndDrain(std::function<void()> teardownBarrier,
                                     std::function<void()> stoppedCallback) {
  if (!beginStop(std::move(teardownBarrier), std::move(stoppedCallback))) {
    return false;
  }
  if (mode_ == ExecutorMode::ManualPump && executor_.isOnExecutor()) {
    executor_.pumpUntilIdle();
  }
  return true;
}

bool JsExecutorBackend::pumpOne() { return executor_.pumpOne(); }

void JsExecutorBackend::pumpUntilIdle() { executor_.pumpUntilIdle(); }

ExecutorState JsExecutorBackend::state() const noexcept {
  return executor_.state();
}

std::size_t JsExecutorBackend::pendingDepth() const noexcept {
  return executor_.pendingDepth();
}

bool JsExecutorBackend::isOwnerThread() const noexcept {
  return executor_.isOnExecutor();
}

} // namespace quickapp::js
