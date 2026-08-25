#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "quickapp/js/abi/runtime_abi_types.h"
#include "quickapp/js/engine/js_engine_service.h"

namespace quickapp::js::event {

class HandlerRegistry final {
 public:
  explicit HandlerRegistry(JsEngineService& engineService) noexcept
      : engineService_(engineService) {}
  HandlerRegistry(const HandlerRegistry&) = delete;
  HandlerRegistry& operator=(const HandlerRegistry&) = delete;

  [[nodiscard]] bool startOnExecutor(JsEnginePort& engine,
                                     const JsContextRef& context) noexcept;
  [[nodiscard]] bool bind(std::string surfaceId, std::string handlerId,
                          std::string methodName, JsValueRef pageVm) noexcept;
  void unbind(std::string_view surfaceId, std::string_view handlerId) noexcept;
  [[nodiscard]] bool dispatchOnExecutor(const abi::JsEventDispatch& dispatch) noexcept;
  void closeSurface(std::string_view surfaceId) noexcept;
  void stopOnExecutor() noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return handlers_.size(); }

 private:
  struct Handler final {
    std::string surfaceId;
    std::string methodName;
    JsValueRef pageVm;
  };

  JsEngineService& engineService_;
  JsEnginePort* engine_{nullptr};
  const JsContextRef* context_{nullptr};
  std::map<std::string, Handler, std::less<>> handlers_;
  bool running_{false};
};

}  // namespace quickapp::js::event
