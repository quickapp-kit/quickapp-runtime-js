#pragma once

#include <optional>

#include "quickapp/js/abi/runtime_abi_types.h"

namespace quickapp::js::abi {

using DecodeCoreResult = Result<CoreInboundMessage, AbiRuntimeError>;
using ValidateCallbackResult = Result<void, AbiRuntimeError>;

[[nodiscard]] DecodeCoreResult
decodeCoreMessage(CoreMessageKind expectedKind, const RuntimeValue &value,
                  const ValueLimits &limits) noexcept;

[[nodiscard]] ValidateCallbackResult
validateJsInboundMessage(const JsInboundMessage &message,
                         const ValueLimits &limits) noexcept;

[[nodiscard]] RuntimeValue encodeEnqueueResult(const EnqueueResult &result);

[[nodiscard]] std::optional<CorrelationKey>
coreCorrelation(const CoreInboundMessage &message);
[[nodiscard]] std::optional<std::string>
coreSurfaceId(const CoreInboundMessage &message);
[[nodiscard]] std::optional<JsCallbackKind>
expectedResultKind(const CoreInboundMessage &message);
[[nodiscard]] bool coreMessageNeedsCorrelation(
    const CoreInboundMessage &message);

[[nodiscard]] std::optional<CorrelationKey>
callbackCorrelation(const JsInboundMessage &message);
[[nodiscard]] std::optional<std::string>
callbackSurfaceId(const JsInboundMessage &message);
[[nodiscard]] bool callbackIsResult(const JsInboundMessage &message);

} // namespace quickapp::js::abi
