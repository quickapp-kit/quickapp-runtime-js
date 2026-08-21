#include "quickapp/js/engine/runtime_value.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace quickapp::js {
namespace {

Result<void, RuntimeValueError> validateNode(const RuntimeValue &value,
                                             const ValueLimits &limits,
                                             std::uint32_t depth,
                                             std::uint32_t &nodes) noexcept {
  if (depth > limits.maxDepth) {
    return Result<void, RuntimeValueError>::failure(
        RuntimeValueError::DepthExceeded);
  }
  if (nodes >= limits.maxNodes) {
    return Result<void, RuntimeValueError>::failure(
        RuntimeValueError::NodeCountExceeded);
  }
  ++nodes;

  if (const auto *number = std::get_if<double>(&value.storage())) {
    if (!std::isfinite(*number)) {
      return Result<void, RuntimeValueError>::failure(
          RuntimeValueError::NonFiniteNumber);
    }
    if (std::trunc(*number) == *number &&
        std::abs(*number) > kMaxWireSafeInteger) {
      return Result<void, RuntimeValueError>::failure(
          RuntimeValueError::UnsafeInteger);
    }
    return Result<void, RuntimeValueError>::success();
  }

  if (const auto *array = std::get_if<RuntimeValue::Array>(&value.storage())) {
    for (const auto &child : *array) {
      auto result = validateNode(child, limits, depth + 1, nodes);
      if (!result.ok()) {
        return result;
      }
    }
  } else if (const auto *object =
                 std::get_if<RuntimeValue::Object>(&value.storage())) {
    for (const auto &[key, child] : *object) {
      static_cast<void>(key);
      auto result = validateNode(child, limits, depth + 1, nodes);
      if (!result.ok()) {
        return result;
      }
    }
  }
  return Result<void, RuntimeValueError>::success();
}

} // namespace

bool isSafeRuntimeNumber(double value) noexcept {
  if (!std::isfinite(value)) {
    return false;
  }
  return std::trunc(value) != value || std::abs(value) <= kMaxWireSafeInteger;
}

Result<void, RuntimeValueError>
validateRuntimeValue(const RuntimeValue &value,
                     const ValueLimits &limits) noexcept {
  if (limits.maxDepth == 0 || limits.maxNodes == 0) {
    return Result<void, RuntimeValueError>::failure(
        RuntimeValueError::DepthExceeded);
  }
  std::uint32_t nodes = 0;
  return validateNode(value, limits, 0, nodes);
}

} // namespace quickapp::js
