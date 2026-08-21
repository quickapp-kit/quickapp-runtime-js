#include "quickapp/js/engine/js_executor.h"

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace quickapp::js {

class JsExecutor::Impl {
public:
  Impl(std::size_t maxPendingTasks, OverflowObserver observer)
      : maxPendingTasks_(maxPendingTasks),
        overflowObserver_(std::move(observer)) {}

  ~Impl() {
    const ExecutorState current = state();
    if (current != ExecutorState::New && current != ExecutorState::Stopped) {
      std::terminate();
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    if (!queue_.empty()) {
      std::terminate();
    }
  }

  bool start(ExecutorMode mode) {
    std::lock_guard lock(mutex_);
    if (state_ != ExecutorState::New || maxPendingTasks_ == 0) {
      return false;
    }
    mode_ = mode;
    state_ = ExecutorState::Running;
    if (mode == ExecutorMode::ManualPump) {
      ownerThread_ = std::this_thread::get_id();
    } else {
      worker_ = std::thread([this] { workerLoop(); });
    }
    return true;
  }

  PostResult post(ExecutorTask task) {
    return postImpl(std::move(task), false);
  }

  PostResult postUniqueMicrotaskContinuation(ExecutorTask task) {
    return postImpl(std::move(task), true);
  }

  PostResult postImpl(ExecutorTask task, bool microtaskContinuation) {
    std::size_t overflowDepth = 0;
    {
      std::lock_guard lock(mutex_);
      if (state_ != ExecutorState::Running) {
        return {PostStatus::Stopping, 0};
      }
      if (microtaskContinuation && microtaskContinuationPending_) {
        return {PostStatus::Accepted, microtaskContinuationSequence_};
      }
      if (queue_.size() >= maxPendingTasks_) {
        overflowDepth = queue_.size();
      } else {
        const auto sequence = nextSequence_++;
        queue_.push_back(
            QueuedTask{sequence, std::move(task), microtaskContinuation});
        if (microtaskContinuation) {
          microtaskContinuationPending_ = true;
          microtaskContinuationSequence_ = sequence;
        }
        cv_.notify_one();
        return {PostStatus::Accepted, sequence};
      }
    }
    if (overflowObserver_) {
      overflowObserver_(overflowDepth);
    }
    return {PostStatus::QueueOverflow, 0};
  }

  bool beginStop(std::function<void()> teardownBarrier,
                 std::function<void()> stoppedCallback) {
    std::lock_guard lock(mutex_);
    if (state_ != ExecutorState::Running) {
      return false;
    }
    state_ = ExecutorState::Quiescing;
    teardownBarrier_ = std::move(teardownBarrier);
    stoppedCallback_ = std::move(stoppedCallback);
    cv_.notify_all();
    return true;
  }

  bool pumpOne() {
    if (mode_ != ExecutorMode::ManualPump || !isOnExecutor()) {
      return false;
    }
    return processOne();
  }

  void pumpUntilIdle() {
    while (pumpOne()) {
    }
  }

  ExecutorState state() const noexcept {
    std::lock_guard lock(mutex_);
    return state_;
  }

  std::size_t pendingDepth() const noexcept {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  bool hasPendingMicrotaskContinuation() const noexcept {
    std::lock_guard lock(mutex_);
    return microtaskContinuationPending_;
  }

  bool isOnExecutor() const noexcept {
    std::lock_guard lock(mutex_);
    return ownerThread_ == std::this_thread::get_id();
  }

  std::thread::id ownerThread() const noexcept {
    std::lock_guard lock(mutex_);
    return ownerThread_;
  }

private:
  struct QueuedTask {
    std::uint64_t sequence;
    ExecutorTask task;
    bool microtaskContinuation;
  };

  void workerLoop() {
    {
      std::lock_guard lock(mutex_);
      ownerThread_ = std::this_thread::get_id();
      cv_.notify_all();
    }
    for (;;) {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] {
        return !queue_.empty() || state_ == ExecutorState::Quiescing ||
               state_ == ExecutorState::Stopped;
      });
      if (state_ == ExecutorState::Stopped) {
        return;
      }
      lock.unlock();
      if (!processOne()) {
        std::this_thread::yield();
      }
    }
  }

  bool processOne() {
    std::optional<QueuedTask> queued;
    std::function<void()> teardown;
    std::function<void()> stopped;
    bool cancel = false;

    {
      std::lock_guard lock(mutex_);
      if (!queue_.empty()) {
        queued.emplace(std::move(queue_.front()));
        queue_.pop_front();
        if (queued->microtaskContinuation) {
          microtaskContinuationPending_ = false;
          microtaskContinuationSequence_ = 0;
        }
        cancel = state_ == ExecutorState::Quiescing;
      } else if (state_ == ExecutorState::Quiescing && !teardownTaken_) {
        teardownTaken_ = true;
        teardown = std::move(teardownBarrier_);
        stopped = std::move(stoppedCallback_);
      } else {
        return false;
      }
    }

    if (queued.has_value()) {
      if (cancel) {
        if (queued->task.onCancelled) {
          try {
            queued->task.onCancelled();
          } catch (...) {
          }
        }
      } else if (queued->task.run) {
        try {
          queued->task.run();
        } catch (...) {
        }
      }
      return true;
    }

    if (teardown) {
      try {
        teardown();
      } catch (...) {
      }
    }
    {
      std::lock_guard lock(mutex_);
      state_ = ExecutorState::Stopped;
      cv_.notify_all();
    }
    if (stopped) {
      try {
        stopped();
      } catch (...) {
      }
    }
    return true;
  }

  const std::size_t maxPendingTasks_;
  OverflowObserver overflowObserver_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<QueuedTask> queue_;
  ExecutorMode mode_{ExecutorMode::ManualPump};
  ExecutorState state_{ExecutorState::New};
  std::thread::id ownerThread_;
  std::thread worker_;
  std::uint64_t nextSequence_{1};
  std::function<void()> teardownBarrier_;
  std::function<void()> stoppedCallback_;
  bool teardownTaken_{false};
  bool microtaskContinuationPending_{false};
  std::uint64_t microtaskContinuationSequence_{0};
};

JsExecutor::JsExecutor(std::size_t maxPendingTasks,
                       OverflowObserver overflowObserver)
    : impl_(std::make_unique<Impl>(maxPendingTasks,
                                   std::move(overflowObserver))) {}

JsExecutor::~JsExecutor() = default;

bool JsExecutor::start(ExecutorMode mode) { return impl_->start(mode); }
PostResult JsExecutor::post(ExecutorTask task) {
  return impl_->post(std::move(task));
}
PostResult JsExecutor::postUniqueMicrotaskContinuation(ExecutorTask task) {
  return impl_->postUniqueMicrotaskContinuation(std::move(task));
}
bool JsExecutor::beginStop(std::function<void()> teardownBarrier,
                           std::function<void()> stoppedCallback) {
  return impl_->beginStop(std::move(teardownBarrier),
                          std::move(stoppedCallback));
}
bool JsExecutor::pumpOne() { return impl_->pumpOne(); }
void JsExecutor::pumpUntilIdle() { impl_->pumpUntilIdle(); }
ExecutorState JsExecutor::state() const noexcept { return impl_->state(); }
std::size_t JsExecutor::pendingDepth() const noexcept {
  return impl_->pendingDepth();
}
bool JsExecutor::hasPendingMicrotaskContinuation() const noexcept {
  return impl_->hasPendingMicrotaskContinuation();
}
bool JsExecutor::isOnExecutor() const noexcept { return impl_->isOnExecutor(); }
std::thread::id JsExecutor::ownerThread() const noexcept {
  return impl_->ownerThread();
}

} // namespace quickapp::js
