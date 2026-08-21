#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "quickapp/js/engine/result.h"

namespace quickapp::js {

constexpr std::uint64_t kMaxWireSafeInteger = 9007199254740991ULL;

enum class RuntimeValueError {
  NonFiniteNumber,
  UnsafeInteger,
  DepthExceeded,
  NodeCountExceeded,
};

struct ValueLimits {
  std::uint32_t maxDepth{64};
  std::uint32_t maxNodes{10000};
};

class RuntimeValue {
public:
  using Array = std::vector<RuntimeValue>;
  using Object = std::map<std::string, RuntimeValue, std::less<>>;
  using Storage =
      std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  RuntimeValue() : storage_(nullptr) {}
  RuntimeValue(std::nullptr_t) : storage_(nullptr) {}
  RuntimeValue(bool value) : storage_(value) {}
  RuntimeValue(double value) : storage_(value) {}
  RuntimeValue(std::string value) : storage_(std::move(value)) {}
  RuntimeValue(const char *value) : storage_(std::string(value)) {}
  RuntimeValue(Array value) : storage_(std::move(value)) {}
  RuntimeValue(Object value) : storage_(std::move(value)) {}

  [[nodiscard]] const Storage &storage() const noexcept { return storage_; }
  [[nodiscard]] Storage &storage() noexcept { return storage_; }

  friend bool operator==(const RuntimeValue &, const RuntimeValue &) = default;

private:
  Storage storage_;
};

[[nodiscard]] bool isSafeRuntimeNumber(double value) noexcept;
[[nodiscard]] Result<void, RuntimeValueError>
validateRuntimeValue(const RuntimeValue &value,
                     const ValueLimits &limits) noexcept;

} // namespace quickapp::js
