#pragma once

#include "quickapp/js/engine/event_loop_backend.h"
#include "quickapp/js/engine/js_executor.h"

namespace quickapp::js {

class JsExecutorBackend final : public EventLoopBackend {
public:
  explicit JsExecutorBackend(std::size_t maxPendingTasks,
                             JsExecutor::OverflowObserver observer = {});
  ~JsExecutorBackend() override;

  [[nodiscard]] bool start(ExecutorMode mode) override;
  [[nodiscard]] PostResult post(ExecutorTask task) override;
  [[nodiscard]] PostResult
  postUniqueMicrotaskContinuation(ExecutorTask task) override;
  [[nodiscard]] bool
  beginStop(std::function<void()> teardownBarrier,
            std::function<void()> stoppedCallback = {}) override;
  [[nodiscard]] bool
  stopAndDrain(std::function<void()> teardownBarrier,
               std::function<void()> stoppedCallback = {}) override;
  [[nodiscard]] bool pumpOne() override;
  void pumpUntilIdle() override;
  [[nodiscard]] ExecutorState state() const noexcept override;
  [[nodiscard]] std::size_t pendingDepth() const noexcept override;
  [[nodiscard]] bool isOwnerThread() const noexcept override;

private:
  JsExecutor executor_;
  ExecutorMode mode_{ExecutorMode::ManualPump};
};

} // namespace quickapp::js
