#pragma once

#include "quickapp/js/abi/runtime_abi_types.h"
#include "quickapp/js/engine/engine_types.h"

namespace quickapp::js::vm {

struct PageVmSetupError {
  std::string code;
  std::string message;
};

using PageVmSetupResult = Result<void, PageVmSetupError>;

class PageVmSetupPort {
public:
  virtual ~PageVmSetupPort() = default;
  [[nodiscard]] virtual PageVmSetupResult
  installOnExecutor(JsValueRef &pageVm,
                    const abi::SurfaceContext &surface) noexcept = 0;
};

} // namespace quickapp::js::vm
