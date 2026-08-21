#include "quickapp/js/engine/js_engine_service.h"

#include <exception>
#include <utility>

namespace quickapp::js {
namespace {

RuntimeError serviceError(RuntimeErrorCode code, std::string message) {
  return RuntimeError{code, std::move(message)};
}

} // namespace

JsEngineService::JsEngineService(std::string appRuntimeId,
                                 std::unique_ptr<JsEngineProvider> provider,
                                 JsEngineConfig engineConfig,
                                 const MonotonicClock &clock,
                                 TraceSinkRegistration sink,
                                 ObservationConfig observationConfig)
    : appRuntimeId_(std::move(appRuntimeId)), provider_(std::move(provider)),
      engineConfig_(std::move(engineConfig)),
      observation_(clock, sink.sink(), std::move(observationConfig)),
      executor_(engineConfig_.limits.maxPendingTasks,
                [this](std::size_t depth) {
                  static_cast<void>(observation_.emitQueueOverflow(depth));
                }) {
  engineConfig_.onOutOfMemory = [this] {
    static_cast<void>(observation_.emitOutOfMemory());
  };
}

JsEngineService::~JsEngineService() {
  const auto current = state();
  if (current != EngineServiceState::New &&
      current != EngineServiceState::Stopped) {
    std::terminate();
  }
}

bool JsEngineService::start(StartCallback callback) {
  EngineServiceState expected = EngineServiceState::New;
  if (!state_.compare_exchange_strong(expected, EngineServiceState::Starting,
                                      std::memory_order_acq_rel)) {
    return false;
  }
  if (!provider_ || engineConfig_.limits.maxHeapBytes == 0 ||
      engineConfig_.limits.maxStackBytes == 0 ||
      engineConfig_.limits.maxPendingTasks == 0 ||
      engineConfig_.limits.maxMicrotasksPerTurn == 0 ||
      engineConfig_.limits.maxRuntimeValueDepth == 0 ||
      engineConfig_.limits.maxRuntimeValueNodes == 0 ||
      !executor_.start(ExecutorMode::OwnedThread)) {
    state_.store(EngineServiceState::Stopped, std::memory_order_release);
    return false;
  }

  const auto posted = executor_.post(ExecutorTask{
      .run =
          [this, callback = std::move(callback)]() mutable {
            initializeOnExecutor(std::move(callback));
          },
      .onCancelled = {},
  });
  if (posted.status != PostStatus::Accepted) {
    state_.store(EngineServiceState::Failed, std::memory_order_release);
    return false;
  }
  return true;
}

PostResult JsEngineService::post(EngineTask task,
                                 std::function<void()> onCancelled) {
  if (state() != EngineServiceState::Running) {
    return {PostStatus::Stopping, 0};
  }
  const auto result = executor_.post(ExecutorTask{
      .run =
          [this, task = std::move(task)]() mutable {
            if (engine_ && context_.valid()) {
              task(*engine_, context_);
            }
          },
      .onCancelled = std::move(onCancelled),
  });
  if (result.status == PostStatus::Accepted) {
    static_cast<void>(observation_.emitQueueDepth(executor_.pendingDepth()));
  }
  return result;
}

PostResult JsEngineService::postOperation(EngineOperation operation,
                                          OperationCallback callback,
                                          std::function<void()> onCancelled) {
  if (state() != EngineServiceState::Running) {
    return {PostStatus::Stopping, 0};
  }
  const auto result = executor_.post(ExecutorTask{
      .run =
          [this, operation = std::move(operation),
           callback = std::move(callback)]() mutable {
            EngineResult<void> operationResult =
                EngineResult<void>::failure(EngineException{
                    EngineExceptionKind::Runtime,
                    "engine operation did not complete", std::nullopt,
                    std::nullopt, std::nullopt, std::nullopt});
            try {
              if (engine_ && context_.valid()) {
                operationResult = operation(*engine_, context_);
              }
            } catch (const std::exception &exception) {
              operationResult = EngineResult<void>::failure(EngineException{
                  EngineExceptionKind::NativeBinding, exception.what(),
                  std::nullopt, std::nullopt, std::nullopt, std::nullopt});
            } catch (...) {
              operationResult = EngineResult<void>::failure(EngineException{
                  EngineExceptionKind::NativeBinding, "engine operation threw",
                  std::nullopt, std::nullopt, std::nullopt, std::nullopt});
            }
            if (!operationResult.ok() && operationResult.error().kind ==
                                             EngineExceptionKind::Terminated) {
              failRunningEngine();
            }
            if (callback) {
              callback(operationResult);
            }
          },
      .onCancelled = std::move(onCancelled),
  });
  if (result.status == PostStatus::Accepted) {
    static_cast<void>(observation_.emitQueueDepth(executor_.pendingDepth()));
  }
  return result;
}

PostResult JsEngineService::checkpointMicrotasks(
    std::function<void(EngineResult<MicrotaskDrain>)> callback) {
  if (state() != EngineServiceState::Running) {
    return {PostStatus::Stopping, 0};
  }
  bool expected = false;
  if (!microtaskDrainActive_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return {PostStatus::Accepted,
            microtaskDrainSequence_.load(std::memory_order_acquire)};
  }
  const auto result = enqueueMicrotaskContinuation(std::move(callback));
  if (result.status == PostStatus::Accepted) {
    microtaskDrainSequence_.store(result.sequence, std::memory_order_release);
  } else {
    microtaskDrainActive_.store(false, std::memory_order_release);
  }
  return result;
}

bool JsEngineService::stop(std::function<void()> upperLayerTeardown,
                           StopCallback callback) {
  EngineServiceState expected = EngineServiceState::Running;
  if (!state_.compare_exchange_strong(expected, EngineServiceState::Quiescing,
                                      std::memory_order_acq_rel)) {
    return false;
  }
  return executor_.beginStop(
      [this, upperLayerTeardown = std::move(upperLayerTeardown)]() mutable {
        teardownOnExecutor(std::move(upperLayerTeardown));
      },
      [this, callback = std::move(callback)]() mutable {
        state_.store(EngineServiceState::Stopped, std::memory_order_release);
        if (callback) {
          callback();
        }
      });
}

EngineServiceState JsEngineService::state() const noexcept {
  return state_.load(std::memory_order_acquire);
}

void JsEngineService::initializeOnExecutor(StartCallback callback) noexcept {
  const auto descriptor = provider_->describe();
  if (descriptor != engineConfig_.expectedEngine ||
      descriptor.engineAbi != "quickapp-kit-js-engine-v1") {
    failStart(serviceError(
                  RuntimeErrorCode::ModuleAbiUnsupported,
                  "selected JS engine does not match the runtime composition"),
              std::move(callback));
    return;
  }

  engine_ = provider_->create(engineConfig_);
  if (!engine_) {
    failStart(serviceError(RuntimeErrorCode::OutOfMemory,
                           "failed to create the selected JS engine"),
              std::move(callback));
    return;
  }
  auto contextResult = engine_->createContext();
  if (!contextResult.ok()) {
    const auto code =
        contextResult.error().kind == EngineExceptionKind::OutOfMemory
            ? RuntimeErrorCode::OutOfMemory
            : RuntimeErrorCode::JsException;
    engine_.reset();
    failStart(serviceError(code, contextResult.error().message),
              std::move(callback));
    return;
  }
  context_ = std::move(contextResult).value();
  state_.store(EngineServiceState::Running, std::memory_order_release);
  if (callback) {
    callback(ServiceResult::success());
  }
}

void JsEngineService::teardownOnExecutor(
    std::function<void()> upperLayerTeardown) noexcept {
  if (upperLayerTeardown) {
    upperLayerTeardown();
  }
  if (engine_ && context_.valid()) {
    static_cast<void>(engine_->destroyContext(context_));
  }
  context_ = JsContextRef{};
  engine_.reset();
}

void JsEngineService::failStart(RuntimeError error,
                                StartCallback callback) noexcept {
  state_.store(EngineServiceState::Failed, std::memory_order_release);
  const bool stopping = executor_.beginStop(
      [this] {
        context_ = JsContextRef{};
        engine_.reset();
      },
      [this, error = std::move(error),
       callback = std::move(callback)]() mutable {
        state_.store(EngineServiceState::Stopped, std::memory_order_release);
        if (callback) {
          callback(ServiceResult::failure(std::move(error)));
        }
      });
  static_cast<void>(stopping);
}

void JsEngineService::failRunningEngine() noexcept {
  EngineServiceState expected = EngineServiceState::Running;
  if (!state_.compare_exchange_strong(expected, EngineServiceState::Failed,
                                      std::memory_order_acq_rel)) {
    return;
  }
  const bool stopping = executor_.beginStop(
      [this] {
        context_ = JsContextRef{};
        engine_.reset();
      },
      [this] {
        state_.store(EngineServiceState::Stopped, std::memory_order_release);
      });
  static_cast<void>(stopping);
}

PostResult JsEngineService::enqueueMicrotaskContinuation(
    std::function<void(EngineResult<MicrotaskDrain>)> callback) {
  return executor_.postUniqueMicrotaskContinuation(ExecutorTask{
      .run =
          [this, callback = std::move(callback)]() mutable {
            auto result = engine_->drainMicrotasks(
                context_, engineConfig_.limits.maxMicrotasksPerTurn);
            const bool pending = result.ok() && result.value().pending;
            if (callback) {
              callback(result);
            }
            if (pending && state() == EngineServiceState::Running) {
              const auto continued =
                  enqueueMicrotaskContinuation(std::move(callback));
              if (continued.status == PostStatus::Accepted) {
                microtaskDrainSequence_.store(continued.sequence,
                                              std::memory_order_release);
              } else {
                microtaskDrainActive_.store(false, std::memory_order_release);
              }
            } else {
              microtaskDrainActive_.store(false, std::memory_order_release);
            }
          },
      .onCancelled =
          [this] {
            microtaskDrainActive_.store(false, std::memory_order_release);
          },
  });
}

} // namespace quickapp::js
