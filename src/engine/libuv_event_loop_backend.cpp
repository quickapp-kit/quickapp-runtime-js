#include "quickapp/js/engine/libuv_event_loop_backend.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <uv.h>

namespace quickapp::js {

class LibuvEventLoopBackend::Impl {
public:
  Impl(std::size_t maxPendingTasks, std::function<void(std::size_t)> observer)
      : maxPendingTasks_(maxPendingTasks), overflowObserver_(std::move(observer)) {}

  ~Impl() {
    const auto current = state();
    if (current != ExecutorState::New && current != ExecutorState::Stopped) {
      std::terminate();
    }
    if (worker_.joinable()) worker_.join();
    if (!queue_.empty()) std::terminate();
  }

  bool start(ExecutorMode mode) {
    std::unique_lock lock(mutex_);
    if (state_ != ExecutorState::New || maxPendingTasks_ == 0) return false;
    mode_ = mode;
    state_ = ExecutorState::Running;
    if (mode == ExecutorMode::ManualPump) {
      ownerThread_ = std::this_thread::get_id();
      lock.unlock();
      if (!initializeLoop()) {
        lock.lock();
        state_ = ExecutorState::Stopped;
        return false;
      }
      return true;
    }
    worker_ = std::thread([this] { workerLoop(); });
    readyCv_.wait(lock, [this] { return initialized_ || initFailed_; });
    return initialized_ && !initFailed_;
  }

  PostResult post(ExecutorTask task) { return postImpl(std::move(task), false); }

  PostResult postUniqueMicrotaskContinuation(ExecutorTask task) {
    return postImpl(std::move(task), true);
  }

  bool beginStop(std::function<void()> teardownBarrier,
                 std::function<void()> stoppedCallback) {
    {
      std::lock_guard lock(mutex_);
      if (state_ != ExecutorState::Running) return false;
      state_ = ExecutorState::Quiescing;
      teardownBarrier_ = std::move(teardownBarrier);
      stoppedCallback_ = std::move(stoppedCallback);
    }
    wake();
    return true;
  }

  bool stopAndDrain(std::function<void()> teardownBarrier,
                    std::function<void()> stoppedCallback) {
    if (!beginStop(std::move(teardownBarrier), std::move(stoppedCallback))) return false;
    if (mode_ == ExecutorMode::ManualPump && isOwnerThread()) {
      pumpUntilIdle();
      return true;
    }
    std::unique_lock lock(mutex_);
    stoppedCv_.wait(lock, [this] { return state_ == ExecutorState::Stopped; });
    return true;
  }

  bool pumpOne() {
    if (mode_ != ExecutorMode::ManualPump || !isOwnerThread()) return false;
    if (initialized_) (void)uv_run(&loop_, UV_RUN_NOWAIT);
    return processOne();
  }

  void pumpUntilIdle() {
    while (pumpOne()) {
    }
    if (mode_ == ExecutorMode::ManualPump && state() == ExecutorState::Stopped) {
      closeLoop();
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

  bool isOwnerThread() const noexcept {
    std::lock_guard lock(mutex_);
    return ownerThread_ == std::this_thread::get_id();
  }

private:
  struct QueuedTask {
    std::uint64_t sequence;
    ExecutorTask task;
    bool microtaskContinuation;
  };

  PostResult postImpl(ExecutorTask task, bool microtaskContinuation) {
    std::size_t overflowDepth = 0;
    {
      std::lock_guard lock(mutex_);
      if (state_ != ExecutorState::Running) return {PostStatus::Stopping, 0};
      if (microtaskContinuation && microtaskContinuationPending_) {
        return {PostStatus::Accepted, microtaskContinuationSequence_};
      }
      if (queue_.size() >= maxPendingTasks_) {
        overflowDepth = queue_.size();
      } else {
        const auto sequence = nextSequence_++;
        queue_.push_back({sequence, std::move(task), microtaskContinuation});
        if (microtaskContinuation) {
          microtaskContinuationPending_ = true;
          microtaskContinuationSequence_ = sequence;
        }
        wake();
        return {PostStatus::Accepted, sequence};
      }
    }
    if (overflowObserver_) overflowObserver_(overflowDepth);
    return {PostStatus::QueueOverflow, 0};
  }

  bool initializeLoop() {
    if (uv_loop_init(&loop_) != 0) return false;
    async_.data = this;
    if (uv_async_init(&loop_, &async_, &Impl::onAsync) != 0) {
      (void)uv_loop_close(&loop_);
      return false;
    }
    initialized_.store(true, std::memory_order_release);
    {
      std::lock_guard lock(mutex_);
      initFailed_.store(false, std::memory_order_release);
    }
    readyCv_.notify_all();
    return true;
  }

  void workerLoop() {
    {
      std::lock_guard lock(mutex_);
      ownerThread_ = std::this_thread::get_id();
    }
    if (!initializeLoop()) {
      {
        std::lock_guard lock(mutex_);
        initFailed_.store(true, std::memory_order_release);
        state_ = ExecutorState::Stopped;
      }
      readyCv_.notify_all();
      stoppedCv_.notify_all();
      return;
    }
    for (;;) {
      if (processOne()) continue;
      {
        std::lock_guard lock(mutex_);
        if (state_ == ExecutorState::Stopped) break;
      }
      (void)uv_run(&loop_, UV_RUN_ONCE);
    }
    closeLoop();
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
    if (queued) {
      if (cancel) {
        if (queued->task.onCancelled) {
          try { queued->task.onCancelled(); } catch (...) {}
        }
      } else if (queued->task.run) {
        try { queued->task.run(); } catch (...) {}
      }
      return true;
    }
    if (teardown) {
      try { teardown(); } catch (...) {}
    }
    {
      std::lock_guard lock(mutex_);
      state_ = ExecutorState::Stopped;
    }
    stoppedCv_.notify_all();
    if (stopped) {
      try { stopped(); } catch (...) {}
    }
    return true;
  }

  void wake() {
    if (initialized_.load(std::memory_order_acquire) &&
        !closed_.load(std::memory_order_acquire)) {
      (void)uv_async_send(&async_);
    }
  }

  void closeLoop() {
    if (!initialized_.load(std::memory_order_acquire) ||
        closed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    uv_close(reinterpret_cast<uv_handle_t*>(&async_), nullptr);
    (void)uv_run(&loop_, UV_RUN_DEFAULT);
    (void)uv_loop_close(&loop_);
  }

  static void onAsync(uv_async_t*) noexcept {}

  const std::size_t maxPendingTasks_;
  std::function<void(std::size_t)> overflowObserver_;
  mutable std::mutex mutex_;
  std::condition_variable readyCv_;
  std::condition_variable stoppedCv_;
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
  uv_loop_t loop_{};
  uv_async_t async_{};
  std::atomic<bool> initialized_{false};
  std::atomic<bool> initFailed_{false};
  std::atomic<bool> closed_{false};
};

LibuvEventLoopBackend::LibuvEventLoopBackend(
    std::size_t maxPendingTasks, std::function<void(std::size_t)> observer)
    : impl_(new Impl(maxPendingTasks, std::move(observer))) {}

LibuvEventLoopBackend::~LibuvEventLoopBackend() { delete impl_; }

bool LibuvEventLoopBackend::start(ExecutorMode mode) { return impl_->start(mode); }
PostResult LibuvEventLoopBackend::post(ExecutorTask task) { return impl_->post(std::move(task)); }
PostResult LibuvEventLoopBackend::postUniqueMicrotaskContinuation(ExecutorTask task) {
  return impl_->postUniqueMicrotaskContinuation(std::move(task));
}
bool LibuvEventLoopBackend::beginStop(std::function<void()> teardownBarrier,
                                      std::function<void()> stoppedCallback) {
  return impl_->beginStop(std::move(teardownBarrier), std::move(stoppedCallback));
}
bool LibuvEventLoopBackend::stopAndDrain(std::function<void()> teardownBarrier,
                                         std::function<void()> stoppedCallback) {
  return impl_->stopAndDrain(std::move(teardownBarrier), std::move(stoppedCallback));
}
bool LibuvEventLoopBackend::pumpOne() { return impl_->pumpOne(); }
void LibuvEventLoopBackend::pumpUntilIdle() { impl_->pumpUntilIdle(); }
ExecutorState LibuvEventLoopBackend::state() const noexcept { return impl_->state(); }
std::size_t LibuvEventLoopBackend::pendingDepth() const noexcept { return impl_->pendingDepth(); }
bool LibuvEventLoopBackend::isOwnerThread() const noexcept { return impl_->isOwnerThread(); }

} // namespace quickapp::js
