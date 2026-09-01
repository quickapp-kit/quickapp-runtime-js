#pragma once

#include <cstddef>
#include <functional>

#include "quickapp/js/engine/event_loop_backend.h"

namespace quickapp::js {

class LibuvEventLoopBackend final : public EventLoopBackend {
public:
  explicit LibuvEventLoopBackend(std::size_t maxPendingTasks,
                                 std::function<void(std::size_t)> observer = {});
  ~LibuvEventLoopBackend() override;

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
  class Impl;
  Impl* impl_;
};

} // namespace quickapp::js
