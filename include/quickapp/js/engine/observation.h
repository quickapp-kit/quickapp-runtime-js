#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "quickapp/js/engine/runtime_value.h"

namespace quickapp::js {

class MonotonicClock {
public:
  virtual ~MonotonicClock() = default;
  [[nodiscard]] virtual std::uint64_t nowNs() const noexcept = 0;
};

class SteadyMonotonicClock final : public MonotonicClock {
public:
  [[nodiscard]] std::uint64_t nowNs() const noexcept override;
};

struct TraceEvent {
  std::uint64_t schemaVersion{1};
  std::string_view kind{"observationMarker"};
  std::string_view runId;
  std::string_view producer{"js"};
  std::string_view markerName;
  std::uint64_t timestampNs{0};
  std::string_view clockDomain;
  std::uint64_t sequence{0};
  std::optional<std::uint64_t> counterValue;
  std::optional<std::string_view> counterName;
  std::optional<std::string_view> errorCode;
  std::optional<std::string_view> appRuntimeId;
  std::optional<std::string_view> surfaceId;
  std::optional<std::string_view> requestId;
  std::optional<std::string_view> transactionId;
};

class TraceSink {
public:
  virtual ~TraceSink() = default;
  virtual void emit(const TraceEvent &event) noexcept = 0;
};

class NoopTraceSink final : public TraceSink {
public:
  void emit(const TraceEvent &) noexcept override {}
};

struct TraceSinkContractDeclaration {
  bool nonblocking{false};
  bool noReentry{false};
};

enum class TraceSinkAdmissionError { MayBlock, MayReenter };

class TraceSinkRegistration {
public:
  [[nodiscard]] static Result<TraceSinkRegistration, TraceSinkAdmissionError>
  admit(TraceSink &sink, TraceSinkContractDeclaration declaration) noexcept;

  [[nodiscard]] TraceSink &sink() const noexcept { return *sink_; }

private:
  explicit TraceSinkRegistration(TraceSink &sink) noexcept : sink_(&sink) {}
  TraceSink *sink_;
};

enum class ObservationEmitResult {
  Emitted,
  Disabled,
  InvalidClock,
  RunRotationRequired,
  WireIntegerOutOfRange,
};

struct ObservationConfig {
  bool enabled{true};
  std::string runId;
  std::string clockDomain;
  std::uint64_t runOriginNs{0};
};

class ObservationEmitter {
public:
  ObservationEmitter(const MonotonicClock &clock, TraceSink &sink,
                     ObservationConfig config) noexcept;

  ObservationEmitter(const ObservationEmitter &) = delete;
  ObservationEmitter &operator=(const ObservationEmitter &) = delete;

  [[nodiscard]] ObservationEmitResult
  emitQueueDepth(std::uint64_t depth) noexcept;
  [[nodiscard]] ObservationEmitResult
  emitQueueOverflow(std::uint64_t depth) noexcept;
  [[nodiscard]] ObservationEmitResult emitOutOfMemory() noexcept;
  [[nodiscard]] ObservationEmitResult emitBridge(
      std::string_view markerName, std::string_view appRuntimeId,
      std::string_view requestId,
      std::optional<std::string_view> surfaceId = std::nullopt,
      std::optional<std::string_view> errorCode = std::nullopt) noexcept;
  [[nodiscard]] bool rotationRequired() const noexcept;

private:
  [[nodiscard]] ObservationEmitResult
  emit(std::string_view markerName, std::optional<std::uint64_t> counterValue,
       std::optional<std::string_view> counterName,
       std::optional<std::string_view> errorCode,
       std::optional<std::string_view> appRuntimeId = std::nullopt,
       std::optional<std::string_view> surfaceId = std::nullopt,
       std::optional<std::string_view> requestId = std::nullopt,
       std::optional<std::string_view> transactionId = std::nullopt) noexcept;

  const MonotonicClock &clock_;
  TraceSink &sink_;
  ObservationConfig config_;
  std::atomic<std::uint64_t> nextSequence_{0};
  std::atomic<bool> rotationRequired_{false};
};

[[nodiscard]] bool isWireSafe(const TraceEvent &event) noexcept;

} // namespace quickapp::js
