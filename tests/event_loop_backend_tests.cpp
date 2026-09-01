#include <cassert>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "quickapp/js/engine/js_executor_backend.h"
#if defined(QUICKAPP_JS_HAS_LIBUV_BACKEND)
#include "quickapp/js/engine/libuv_event_loop_backend.h"
#endif

namespace quickapp::js {

using BackendFactory = std::function<std::unique_ptr<EventLoopBackend>(std::size_t)>;

void test_fifo_owner_and_microtask(const BackendFactory &make_backend) {
  auto backend = make_backend(4);
  assert(backend->start(ExecutorMode::ManualPump));
  assert(backend->isOwnerThread());
  bool otherThreadIsOwner = true;
  std::thread observer([&] { otherThreadIsOwner = backend->isOwnerThread(); });
  observer.join();
  assert(!otherThreadIsOwner);
  std::vector<int> order;
  const auto first = backend->postUniqueMicrotaskContinuation(
      ExecutorTask{.run = [&] { order.push_back(1); }});
  const auto duplicate = backend->postUniqueMicrotaskContinuation(
      ExecutorTask{.run = [&] { order.push_back(99); }});
  assert(first.status == PostStatus::Accepted);
  assert(duplicate.status == PostStatus::Accepted);
  assert(first.sequence == duplicate.sequence);
  assert(backend->post(ExecutorTask{.run = [&] { order.push_back(2); }}).status ==
         PostStatus::Accepted);
  backend->pumpUntilIdle();
  assert((order == std::vector<int>{1, 2}));
  assert(backend->stopAndDrain([&] {}));
}

void test_backpressure_and_stop_drain(const BackendFactory &make_backend) {
  auto backend = make_backend(1);
  assert(backend->start(ExecutorMode::ManualPump));
  assert(backend->post(ExecutorTask{}).status == PostStatus::Accepted);
  assert(backend->post(ExecutorTask{}).status == PostStatus::QueueOverflow);
  assert(backend->stopAndDrain([&] {}));
  assert(backend->state() == ExecutorState::Stopped);
  assert(backend->pendingDepth() == 0);
  assert(backend->post(ExecutorTask{}).status == PostStatus::Stopping);
}

void test_quiescing_cancels_queued_work(const BackendFactory &make_backend) {
  auto backend = make_backend(2);
  assert(backend->start(ExecutorMode::ManualPump));
  int cancelled = 0;
  assert(backend->post(ExecutorTask{.onCancelled = [&] { ++cancelled; }}).status ==
         PostStatus::Accepted);
  assert(backend->beginStop([&] {}));
  backend->pumpUntilIdle();
  assert(cancelled == 1);
  assert(backend->state() == ExecutorState::Stopped);
}

void test_owned_thread_and_wakeup(const BackendFactory &make_backend) {
  auto backend = make_backend(4);
  assert(backend->start(ExecutorMode::OwnedThread));
  std::promise<bool> completed;
  auto completedFuture = completed.get_future();
  assert(backend->post(ExecutorTask{.run = [&] {
    completed.set_value(backend->isOwnerThread());
  }}).status == PostStatus::Accepted);
  assert(completedFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  assert(completedFuture.get());
  std::promise<void> stopped;
  auto stoppedFuture = stopped.get_future();
  assert(backend->stopAndDrain([&] {}, [&] { stopped.set_value(); }));
  assert(stoppedFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  assert(backend->state() == ExecutorState::Stopped);
  assert(backend->pendingDepth() == 0);
}

} // namespace quickapp::js

int main() {
  const quickapp::js::BackendFactory js_executor = [](std::size_t capacity) {
    return std::make_unique<quickapp::js::JsExecutorBackend>(capacity);
  };
  quickapp::js::test_fifo_owner_and_microtask(js_executor);
  quickapp::js::test_backpressure_and_stop_drain(js_executor);
  quickapp::js::test_quiescing_cancels_queued_work(js_executor);
  quickapp::js::test_owned_thread_and_wakeup(js_executor);
#if defined(QUICKAPP_JS_HAS_LIBUV_BACKEND)
  const quickapp::js::BackendFactory libuv = [](std::size_t capacity) {
    return std::make_unique<quickapp::js::LibuvEventLoopBackend>(capacity);
  };
  quickapp::js::test_fifo_owner_and_microtask(libuv);
  quickapp::js::test_backpressure_and_stop_drain(libuv);
  quickapp::js::test_quiescing_cancels_queued_work(libuv);
  quickapp::js::test_owned_thread_and_wakeup(libuv);
#endif
  return 0;
}
