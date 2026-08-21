#pragma once

#include "quickapp/js/engine/js_engine_port.h"

namespace quickapp::js {

class QuickJsEngineProvider final : public JsEngineProvider {
public:
  [[nodiscard]] JsEngineDescriptor describe() const noexcept override;
  [[nodiscard]] std::unique_ptr<JsEnginePort>
  create(const JsEngineConfig &config) noexcept override;
};

} // namespace quickapp::js
