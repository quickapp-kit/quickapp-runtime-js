#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include "quickapp/js/engine/event_loop_backend.h"

namespace quickapp::js {

class JsExecutor {
public:
  using OverflowObserver = std::function<void(std::size_t)>;

  explicit JsExecutor(std::size_t maxPendingTasks,
                      OverflowObserver overflowObserver = {});
  ~JsExecutor();

  JsExecutor(const JsExecutor &) = delete;
  JsExecutor &operator=(const JsExecutor &) = delete;

  [[nodiscard]] bool start(ExecutorMode mode);
  [[nodiscard]] PostResult post(ExecutorTask task);
  [[nodiscard]] PostResult postUniqueMicrotaskContinuation(ExecutorTask task);
  [[nodiscard]] bool beginStop(std::function<void()> teardownBarrier,
                               std::function<void()> stoppedCallback = {});

  [[nodiscard]] bool pumpOne();
  void pumpUntilIdle();

  [[nodiscard]] ExecutorState state() const noexcept;
  [[nodiscard]] std::size_t pendingDepth() const noexcept;
  [[nodiscard]] bool hasPendingMicrotaskContinuation() const noexcept;
  [[nodiscard]] bool isOnExecutor() const noexcept;
  [[nodiscard]] std::thread::id ownerThread() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace quickapp::js
