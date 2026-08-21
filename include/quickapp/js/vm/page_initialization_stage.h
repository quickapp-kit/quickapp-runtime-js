#pragma once

#include <string>
#include <string_view>

#include "quickapp/js/engine/result.h"
#include "quickapp/js/module/module_loader.h"

namespace quickapp::js::vm {

struct PageInitializationStageError {
  std::string code;
  std::string message;
};

using PageInitializationStageResult =
    Result<void, PageInitializationStageError>;

// S04 owns lifecycle ordering; stage implementations own binding and render work.
class PageInitializationStagePort {
public:
  virtual ~PageInitializationStagePort() = default;

  [[nodiscard]] virtual PageInitializationStageResult
  evaluateInitialOnExecutor(
      std::string_view surfaceId, std::string_view templateId,
      const module::ModuleDefinitionHandle &definition,
      const JsValueRef &pageVm) noexcept = 0;
  [[nodiscard]] virtual PageInitializationStageResult
  submitInitialOnExecutor(std::string_view surfaceId) noexcept = 0;
  virtual void cancelOnExecutor(std::string_view surfaceId) noexcept = 0;
  virtual void cancelAllOnExecutor() noexcept = 0;
};

} // namespace quickapp::js::vm
