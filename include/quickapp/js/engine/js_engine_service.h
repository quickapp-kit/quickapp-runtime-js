#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "quickapp/js/engine/js_engine_port.h"
#include "quickapp/js/engine/js_executor.h"
#include "quickapp/js/engine/observation.h"

namespace quickapp::js {

enum class EngineServiceState {
  New,
  Starting,
  Running,
  Quiescing,
  Failed,
  Stopped
};

using ServiceResult = Result<void, RuntimeError>;

class JsEngineService {
public:
  using StartCallback = std::function<void(ServiceResult)>;
  using StopCallback = std::function<void()>;
  using EngineTask = std::function<void(JsEnginePort &, const JsContextRef &)>;
  using EngineOperation =
      std::function<EngineResult<void>(JsEnginePort &, const JsContextRef &)>;
  using OperationCallback = std::function<void(const EngineResult<void> &)>;

  JsEngineService(std::string appRuntimeId,
                  std::unique_ptr<JsEngineProvider> provider,
                  JsEngineConfig engineConfig, const MonotonicClock &clock,
                  TraceSinkRegistration sink,
                  ObservationConfig observationConfig);
  ~JsEngineService();

  JsEngineService(const JsEngineService &) = delete;
  JsEngineService &operator=(const JsEngineService &) = delete;

  [[nodiscard]] bool start(StartCallback callback);
  [[nodiscard]] PostResult post(EngineTask task,
                                std::function<void()> onCancelled = {});
  [[nodiscard]] PostResult
  postOperation(EngineOperation operation, OperationCallback callback = {},
                std::function<void()> onCancelled = {});
  [[nodiscard]] PostResult checkpointMicrotasks(
      std::function<void(EngineResult<MicrotaskDrain>)> callback = {});
  [[nodiscard]] bool stop(std::function<void()> upperLayerTeardown,
                          StopCallback callback = {});

  [[nodiscard]] EngineServiceState state() const noexcept;
  [[nodiscard]] const std::string &appRuntimeId() const noexcept {
    return appRuntimeId_;
  }
  [[nodiscard]] ObservationEmitter &observation() noexcept {
    return observation_;
  }
  [[nodiscard]] const JsExecutor &executor() const noexcept {
    return executor_;
  }

private:
  void initializeOnExecutor(StartCallback callback) noexcept;
  void teardownOnExecutor(std::function<void()> upperLayerTeardown) noexcept;
  void failStart(RuntimeError error, StartCallback callback) noexcept;
  void failRunningEngine() noexcept;
  [[nodiscard]] PostResult enqueueMicrotaskContinuation(
      std::function<void(EngineResult<MicrotaskDrain>)> callback);

  std::string appRuntimeId_;
  std::unique_ptr<JsEngineProvider> provider_;
  JsEngineConfig engineConfig_;
  ObservationEmitter observation_;
  JsExecutor executor_;
  std::unique_ptr<JsEnginePort> engine_;
  JsContextRef context_;
  std::atomic<EngineServiceState> state_{EngineServiceState::New};
  std::atomic<bool> microtaskDrainActive_{false};
  std::atomic<std::uint64_t> microtaskDrainSequence_{0};
};

} // namespace quickapp::js
