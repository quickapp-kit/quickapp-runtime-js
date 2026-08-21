#pragma once

#include "quickapp/js/module/module_loader.h"

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
  void stopOnExecutor() noexcept;

private:
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  JsValueRef routerFacade_;
  JsValueRef promptFacade_;
  JsValueRef fetchFacade_;
  bool running_{false};
};

} // namespace quickapp::js::framework
