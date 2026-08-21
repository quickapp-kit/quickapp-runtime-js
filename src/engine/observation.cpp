#include "quickapp/js/engine/observation.h"

#include <chrono>

namespace quickapp::js {

std::uint64_t SteadyMonotonicClock::nowNs() const noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

Result<TraceSinkRegistration, TraceSinkAdmissionError>
TraceSinkRegistration::admit(
    TraceSink &sink, TraceSinkContractDeclaration declaration) noexcept {
  if (!declaration.nonblocking) {
    return Result<TraceSinkRegistration, TraceSinkAdmissionError>::failure(
        TraceSinkAdmissionError::MayBlock);
  }
  if (!declaration.noReentry) {
    return Result<TraceSinkRegistration, TraceSinkAdmissionError>::failure(
        TraceSinkAdmissionError::MayReenter);
  }
  return Result<TraceSinkRegistration, TraceSinkAdmissionError>::success(
      TraceSinkRegistration(sink));
}

ObservationEmitter::ObservationEmitter(const MonotonicClock &clock,
                                       TraceSink &sink,
                                       ObservationConfig config) noexcept
    : clock_(clock), sink_(sink), config_(std::move(config)) {}

ObservationEmitResult
ObservationEmitter::emitQueueDepth(std::uint64_t depth) noexcept {
  return emit("runtime.counter.sampled", depth, "queue.depth", std::nullopt);
}

ObservationEmitResult
ObservationEmitter::emitQueueOverflow(std::uint64_t depth) noexcept {
  return emit("queue.overflow", depth, "queue.depth", "QUEUE_OVERFLOW");
}

ObservationEmitResult ObservationEmitter::emitOutOfMemory() noexcept {
  return emit("runtime.oom", std::nullopt, std::nullopt, "OUT_OF_MEMORY");
}

ObservationEmitResult ObservationEmitter::emitBridge(
    std::string_view markerName, std::string_view appRuntimeId,
    std::string_view requestId, std::optional<std::string_view> surfaceId,
    std::optional<std::string_view> errorCode) noexcept {
  return emit(markerName, std::nullopt, std::nullopt, errorCode, appRuntimeId,
              surfaceId, requestId);
}

bool ObservationEmitter::rotationRequired() const noexcept {
  return rotationRequired_.load(std::memory_order_acquire);
}

ObservationEmitResult
ObservationEmitter::emit(std::string_view markerName,
                         std::optional<std::uint64_t> counterValue,
                         std::optional<std::string_view> counterName,
                         std::optional<std::string_view> errorCode,
                         std::optional<std::string_view> appRuntimeId,
                         std::optional<std::string_view> surfaceId,
                         std::optional<std::string_view> requestId,
                         std::optional<std::string_view> transactionId) noexcept {
  if (!config_.enabled) {
    return ObservationEmitResult::Disabled;
  }
  if (rotationRequired()) {
    return ObservationEmitResult::RunRotationRequired;
  }

  const std::uint64_t now = clock_.nowNs();
  if (now < config_.runOriginNs) {
    return ObservationEmitResult::InvalidClock;
  }
  const std::uint64_t timestamp = now - config_.runOriginNs;
  if (timestamp > kMaxWireSafeInteger ||
      (counterValue.has_value() && *counterValue > kMaxWireSafeInteger)) {
    rotationRequired_.store(true, std::memory_order_release);
    return ObservationEmitResult::WireIntegerOutOfRange;
  }

  const std::uint64_t sequence =
      nextSequence_.fetch_add(1, std::memory_order_acq_rel);
  if (sequence > kMaxWireSafeInteger) {
    rotationRequired_.store(true, std::memory_order_release);
    return ObservationEmitResult::RunRotationRequired;
  }

  const TraceEvent event{
      .schemaVersion = 1,
      .kind = "observationMarker",
      .runId = config_.runId,
      .producer = "js",
      .markerName = markerName,
      .timestampNs = timestamp,
      .clockDomain = config_.clockDomain,
      .sequence = sequence,
      .counterValue = counterValue,
      .counterName = counterName,
      .errorCode = errorCode,
      .appRuntimeId = appRuntimeId,
      .surfaceId = surfaceId,
      .requestId = requestId,
      .transactionId = transactionId,
  };
  if (!isWireSafe(event)) {
    rotationRequired_.store(true, std::memory_order_release);
    return ObservationEmitResult::WireIntegerOutOfRange;
  }
  sink_.emit(event);
  return ObservationEmitResult::Emitted;
}

bool isWireSafe(const TraceEvent &event) noexcept {
  if (event.schemaVersion > kMaxWireSafeInteger ||
      event.timestampNs > kMaxWireSafeInteger ||
      event.sequence > kMaxWireSafeInteger) {
    return false;
  }
  return !event.counterValue.has_value() ||
         *event.counterValue <= kMaxWireSafeInteger;
}

} // namespace quickapp::js
