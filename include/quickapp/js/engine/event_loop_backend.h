#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace quickapp::js {

enum class ExecutorMode { OwnedThread, ManualPump };
enum class ExecutorState { New, Running, Quiescing, Stopped };
enum class PostStatus { Accepted, QueueOverflow, Stopping };

struct PostResult {
  PostStatus status{PostStatus::Stopping};
  std::uint64_t sequence{0};
};

struct ExecutorTask {
  std::function<void()> run;
  std::function<void()> onCancelled;
};

class EventLoopBackend {
public:
  virtual ~EventLoopBackend() = default;

  EventLoopBackend(const EventLoopBackend &) = delete;
  EventLoopBackend &operator=(const EventLoopBackend &) = delete;

  [[nodiscard]] virtual bool start(ExecutorMode mode) = 0;
  [[nodiscard]] virtual PostResult post(ExecutorTask task) = 0;
  [[nodiscard]] virtual PostResult
  postUniqueMicrotaskContinuation(ExecutorTask task) = 0;
  [[nodiscard]] virtual bool
  beginStop(std::function<void()> teardownBarrier,
            std::function<void()> stoppedCallback = {}) = 0;
  [[nodiscard]] virtual bool
  stopAndDrain(std::function<void()> teardownBarrier,
               std::function<void()> stoppedCallback = {}) = 0;
  [[nodiscard]] virtual bool pumpOne() = 0;
  virtual void pumpUntilIdle() = 0;
  [[nodiscard]] virtual ExecutorState state() const noexcept = 0;
  [[nodiscard]] virtual std::size_t pendingDepth() const noexcept = 0;
  [[nodiscard]] virtual bool isOwnerThread() const noexcept = 0;

  [[nodiscard]] bool isOnExecutor() const noexcept { return isOwnerThread(); }

protected:
  EventLoopBackend() = default;
};

} // namespace quickapp::js
