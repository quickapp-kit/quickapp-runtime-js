#pragma once

#include "quickapp/js/module/module_loader.h"

namespace quickapp::js::abi {
struct TimerStartResult;
struct TimerCancelResult;
struct TimerFired;
struct FeatureResult;
}  // namespace quickapp::js::abi

namespace quickapp::js::framework {

class StaticFacadeCatalog final : public module::FrameworkModuleResolverPort {
public:
  StaticFacadeCatalog() = default;
  ~StaticFacadeCatalog();

  StaticFacadeCatalog(const StaticFacadeCatalog &) = delete;
  StaticFacadeCatalog &operator=(const StaticFacadeCatalog &) = delete;

  [[nodiscard]] bool startOnExecutor(JsEnginePort &engine,
                                     const JsContextRef &context) noexcept;
  [[nodiscard]] Result<JsValueRef, module::ModuleError>
  resolveOnExecutor(std::string_view moduleId) noexcept override;
  [[nodiscard]] bool dispatchTimerStartResultOnExecutor(
      const abi::TimerStartResult& result) noexcept;
  [[nodiscard]] bool dispatchTimerCancelResultOnExecutor(
      const abi::TimerCancelResult& result) noexcept;
  [[nodiscard]] bool dispatchTimerFiredOnExecutor(
      const abi::TimerFired& event) noexcept;
  [[nodiscard]] bool dispatchFeatureResultOnExecutor(
      const abi::FeatureResult& result) noexcept;
  void stopOnExecutor() noexcept;

private:
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  JsValueRef routerFacade_;
  JsValueRef promptFacade_;
  JsValueRef deviceFacade_;
  JsValueRef timerFacade_;
  JsValueRef fetchFacade_;
  JsValueRef fileFacade_;
  JsValueRef openUrlFacade_;
  JsValueRef webviewFacade_;
  bool running_{false};
};

} // namespace quickapp::js::framework
