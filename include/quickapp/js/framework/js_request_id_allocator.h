#pragma once

#include <string>

namespace quickapp::js::framework {

// The JS Framework creates one instance per AppRuntime during bootstrap.
// Request producers borrow this port; no producer owns the sequence.
class JsRequestIdAllocatorPort {
public:
  virtual ~JsRequestIdAllocatorPort() = default;
  [[nodiscard]] virtual std::string nextRequestId() noexcept = 0;
};

} // namespace quickapp::js::framework
