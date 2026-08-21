#pragma once

#include <optional>
#include <utility>
#include <variant>

namespace quickapp::js {

template <typename T, typename E> class Result {
public:
  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(E error) {
    return Result(std::move(error), FailureTag{});
  }

  [[nodiscard]] bool ok() const noexcept {
    return std::holds_alternative<T>(storage_);
  }
  [[nodiscard]] T &value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T &value() const & { return std::get<T>(storage_); }
  [[nodiscard]] T &&value() && { return std::get<T>(std::move(storage_)); }
  [[nodiscard]] E &error() & { return std::get<E>(storage_); }
  [[nodiscard]] const E &error() const & { return std::get<E>(storage_); }

private:
  struct FailureTag {};

  explicit Result(T value) : storage_(std::move(value)) {}
  Result(E error, FailureTag) : storage_(std::move(error)) {}

  std::variant<T, E> storage_;
};

template <typename E> class Result<void, E> {
public:
  static Result success() { return Result(); }
  static Result failure(E error) { return Result(std::move(error)); }

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] E &error() & { return *error_; }
  [[nodiscard]] const E &error() const & { return *error_; }

private:
  Result() = default;
  explicit Result(E error) : error_(std::move(error)) {}

  std::optional<E> error_;
};

} // namespace quickapp::js
