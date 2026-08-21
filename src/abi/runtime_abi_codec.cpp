#include "quickapp/js/abi/runtime_abi_codec.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <type_traits>
#include <utility>

namespace quickapp::js::abi {
namespace {

AbiRuntimeError invalid(std::string message) {
  return {AbiErrorCode::InvalidArgument, std::move(message), false,
          std::nullopt, std::nullopt, std::nullopt, std::nullopt};
}

AbiRuntimeError unsupported() {
  return {AbiErrorCode::UnsupportedVersion, "unsupported Runtime ABI version",
          false, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
}

const RuntimeValue *field(const RuntimeValue::Object &object,
                          std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

const std::string *stringField(const RuntimeValue::Object &object,
                               std::string_view name) {
  const auto *value = field(object, name);
  return value ? std::get_if<std::string>(&value->storage()) : nullptr;
}

const RuntimeValue::Object *objectField(const RuntimeValue::Object &object,
                                        std::string_view name) {
  const auto *value = field(object, name);
  return value ? std::get_if<RuntimeValue::Object>(&value->storage()) : nullptr;
}

const RuntimeValue::Array *arrayField(const RuntimeValue::Object &object,
                                      std::string_view name) {
  const auto *value = field(object, name);
  return value ? std::get_if<RuntimeValue::Array>(&value->storage()) : nullptr;
}

bool isNumber(const RuntimeValue *value) {
  return value && std::holds_alternative<double>(value->storage());
}

bool isInteger(const RuntimeValue *value, std::uint64_t minimum) {
  if (!isNumber(value)) {
    return false;
  }
  const double number = std::get<double>(value->storage());
  return std::isfinite(number) && std::floor(number) == number && number >= 0 &&
         static_cast<std::uint64_t>(number) >= minimum &&
         number <= static_cast<double>(kMaxWireSafeInteger);
}

bool hasPrefix(std::string_view value, std::string_view prefix) {
  return value.size() > prefix.size() && value.starts_with(prefix);
}

bool positiveDecimal(std::string_view value) {
  if (value.empty() || value.front() == '0') {
    return false;
  }
  return std::ranges::all_of(value,
                             [](char c) { return c >= '0' && c <= '9'; });
}

bool validJsRequestId(std::string_view value) {
  constexpr std::string_view prefix = "req:j-";
  return value.starts_with(prefix) && positiveDecimal(value.substr(prefix.size()));
}

bool validCoreRequestId(std::string_view value) {
  constexpr std::string_view prefix = "req:";
  return value.starts_with(prefix) && positiveDecimal(value.substr(prefix.size()));
}

bool validPlatformRequestId(std::string_view value) {
  constexpr std::string_view prefix = "req:p-";
  return value.starts_with(prefix) && positiveDecimal(value.substr(prefix.size()));
}

bool exactFields(const RuntimeValue::Object &object,
                 std::initializer_list<std::string_view> required,
                 std::initializer_list<std::string_view> optional = {}) {
  std::set<std::string_view> allowed(required.begin(), required.end());
  allowed.insert(optional.begin(), optional.end());
  for (const auto requiredName : required) {
    if (!object.contains(requiredName)) {
      return false;
    }
  }
  return std::ranges::all_of(object, [&](const auto &entry) {
    return allowed.contains(entry.first);
  });
}

bool stringIn(const RuntimeValue::Object &object, std::string_view name,
              std::initializer_list<std::string_view> allowed) {
  const auto *value = stringField(object, name);
  return value && std::ranges::find(allowed, *value) != allowed.end();
}

bool validSchemaAndKind(const RuntimeValue::Object &object,
                        std::string_view kind, bool hasKind = true) {
  if (!isInteger(field(object, "schemaVersion"), 1) ||
      std::get<double>(field(object, "schemaVersion")->storage()) != 1.0) {
    return false;
  }
  return !hasKind || (stringField(object, "kind") &&
                      *stringField(object, "kind") == kind);
}

bool validSurface(const RuntimeValue::Object &object, std::string_view name) {
  const auto *value = stringField(object, name);
  return value && hasPrefix(*value, "srf:");
}

bool validRequest(const RuntimeValue::Object &object, bool jsOrigin) {
  const auto *requestId = stringField(object, "requestId");
  return requestId && (jsOrigin ? validJsRequestId(*requestId)
                                : validCoreRequestId(*requestId));
}

bool validErrorObject(const RuntimeValue::Object &object) {
  if (!exactFields(object, {"code", "message", "retryable"},
                   {"surfaceId", "requestId", "transactionId",
                    "mountAttemptId"}) ||
      !stringField(object, "code") || !stringField(object, "message") ||
      !field(object, "retryable") ||
      !std::holds_alternative<bool>(field(object, "retryable")->storage())) {
    return false;
  }
  static const std::set<std::string, std::less<>> codes{
      "ABI_INVALID_ARGUMENT", "ABI_UNSUPPORTED_VERSION", "SURFACE_NOT_FOUND",
      "SURFACE_DEGRADED", "SURFACE_FAILED", "SURFACE_HOST_ALREADY_EXISTS",
      "SURFACE_HOST_NOT_FOUND", "SURFACE_PRESENTATION_FAILED",
      "REVISION_STALE", "TARGET_NOT_FOUND", "INVALID_PARENT",
      "BLOCK_NOT_FOUND", "HANDLER_NOT_FOUND", "HANDLER_ALREADY_EXISTS",
      "ROUTE_NOT_FOUND", "NAVIGATION_BUSY", "LIFECYCLE_BUSY",
      "NAVIGATION_FAILED", "PACKAGE_NOT_FOUND",
      "PACKAGE_FORMAT_UNSUPPORTED", "PACKAGE_VERSION_UNSUPPORTED",
      "PACKAGE_IO_ERROR", "PACKAGE_ENTRY_INVALID", "PACKAGE_INTEGRITY_FAILED",
      "PACKAGE_SIGNATURE_REQUIRED", "PACKAGE_SIGNATURE_INVALID",
      "PACKAGE_SIGNER_UNTRUSTED", "PACKAGE_INVALID", "IR_INVALID",
      "TEMPLATE_NOT_FOUND", "MODULE_ABI_UNSUPPORTED",
      "RUNTIME_PROFILE_INCOMPATIBLE", "CAPABILITY_NOT_DECLARED",
      "CAPABILITY_DENIED", "CAPABILITY_UNSUPPORTED", "CAPABILITY_FAILED",
      "HOST_FEATURE_UNSUPPORTED", "MEASURE_FAILED", "OUT_OF_MEMORY",
      "QUEUE_OVERFLOW", "JS_EXCEPTION", "PLATFORM_REJECTED"};
  if (!codes.contains(*stringField(object, "code"))) {
    return false;
  }
  const auto checkOptional = [&](std::string_view name,
                                 std::string_view prefix) {
    const auto *value = field(object, name);
    if (!value) {
      return true;
    }
    const auto *text = std::get_if<std::string>(&value->storage());
    return text && hasPrefix(*text, prefix);
  };
  return checkOptional("surfaceId", "srf:") &&
         checkOptional("requestId", "req:") &&
         checkOptional("transactionId", "txn:") &&
         checkOptional("mountAttemptId", "mnt:");
}

bool validOptionalError(const RuntimeValue::Object &object, bool required) {
  const auto *error = objectField(object, "error");
  return required ? error && validErrorObject(*error) : error == nullptr;
}

bool validHandlerBinding(const RuntimeValue &value, bool blockOwner) {
  const auto *object = std::get_if<RuntimeValue::Object>(&value.storage());
  if (!object || !exactFields(*object,
                              {"ownerInstanceId", "templateHandlerId",
                               "handlerId"})) {
    return false;
  }
  const auto *owner = stringField(*object, "ownerInstanceId");
  const auto *handler = stringField(*object, "handlerId");
  return owner && (blockOwner ? (hasPrefix(*owner, "cmp:") ||
                                hasPrefix(*owner, "blk:"))
                             : hasPrefix(*owner, "cmp:")) &&
         handler && hasPrefix(*handler, "hdl:") &&
         isInteger(field(*object, "templateHandlerId"), 1);
}

bool validBindingMap(const RuntimeValue::Object &bindings) {
  for (const auto &[key, value] : bindings) {
    if (!positiveDecimal(key) ||
        (!std::holds_alternative<std::string>(value.storage()) &&
         !std::holds_alternative<bool>(value.storage()))) {
      return false;
    }
  }
  return true;
}

bool validLogicalNode(const RuntimeValue::Object &object) {
  if (!exactFields(object, {"ownerInstanceId", "templateNodeId"})) {
    return false;
  }
  const auto *owner = stringField(object, "ownerInstanceId");
  return owner && (hasPrefix(*owner, "cmp:") || hasPrefix(*owner, "blk:")) &&
         isInteger(field(object, "templateNodeId"), 1);
}

bool validRenderOperation(const RuntimeValue &value) {
  const auto *object = std::get_if<RuntimeValue::Object>(&value.storage());
  if (!object) {
    return false;
  }
  const auto *kind = stringField(*object, "kind");
  if (!kind) {
    return false;
  }
  if (*kind == "removeBlock") {
    return exactFields(*object, {"kind", "blockInstanceId"}) &&
           stringField(*object, "blockInstanceId") &&
           hasPrefix(*stringField(*object, "blockInstanceId"), "blk:");
  }
  if (*kind == "moveBlock") {
    const auto *parent = objectField(*object, "parent");
    return exactFields(*object,
                       {"kind", "blockInstanceId", "parent", "index"}) &&
           stringField(*object, "blockInstanceId") && parent &&
           validLogicalNode(*parent) && isInteger(field(*object, "index"), 0);
  }
  if (*kind == "updateBinding") {
    const auto *owner = stringField(*object, "ownerInstanceId");
    const auto *bindingValue = field(*object, "value");
    return exactFields(*object,
                       {"kind", "ownerInstanceId", "templateBindingId",
                        "value"}) &&
           owner && (hasPrefix(*owner, "cmp:") || hasPrefix(*owner, "blk:")) &&
           isInteger(field(*object, "templateBindingId"), 1) && bindingValue &&
           (std::holds_alternative<std::string>(bindingValue->storage()) ||
            std::holds_alternative<bool>(bindingValue->storage()));
  }
  if (*kind == "instantiateBlock") {
    const auto *parent = objectField(*object, "parent");
    const auto *bindings = objectField(*object, "initialBindings");
    const auto *handlers = arrayField(*object, "handlers");
    if (!exactFields(*object,
                     {"kind", "templateBlockId", "blockInstanceId", "parent",
                      "index", "initialBindings", "handlers"},
                     {"key"}) ||
        !isInteger(field(*object, "templateBlockId"), 1) || !parent ||
        !validLogicalNode(*parent) || !bindings || !validBindingMap(*bindings) ||
        !handlers || !isInteger(field(*object, "index"), 0)) {
      return false;
    }
    const auto *blockId = stringField(*object, "blockInstanceId");
    return blockId && hasPrefix(*blockId, "blk:") &&
           std::ranges::all_of(*handlers, [](const RuntimeValue &handler) {
             return validHandlerBinding(handler, true);
           });
  }
  return false;
}

bool validateCoreObject(CoreMessageKind kind,
                        const RuntimeValue::Object &object) {
  const auto jsRequest = [&] { return validRequest(object, true); };
  const auto coreCompletion = [&] { return validRequest(object, false); };
  switch (kind) {
  case CoreMessageKind::InstantiateTemplate: {
    const auto *bindings = objectField(object, "initialBindings");
    const auto *blocks = arrayField(object, "initialBlocks");
    const auto *handlers = arrayField(object, "initialHandlers");
    const auto *owner = stringField(object, "ownerInstanceId");
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId", "surfaceId",
                        "templateId", "ownerInstanceId", "initialBindings",
                        "initialBlocks", "initialHandlers"}) &&
           validSchemaAndKind(object, "instantiateTemplate") && jsRequest() &&
           validSurface(object, "surfaceId") &&
           stringField(object, "templateId") &&
           !stringField(object, "templateId")->empty() && owner &&
           hasPrefix(*owner, "cmp:") && bindings && validBindingMap(*bindings) &&
           blocks && std::ranges::all_of(*blocks, validRenderOperation) &&
           handlers &&
           std::ranges::all_of(*handlers, [](const RuntimeValue &handler) {
             return validHandlerBinding(handler, false);
           });
  }
  case CoreMessageKind::CompleteVerifiedModuleLoad: {
    if (!exactFields(object,
                     {"schemaVersion", "kind", "requestId", "moduleKind",
                      "moduleId", "status"},
                     {"surfaceId", "error"}) ||
        !validSchemaAndKind(object, "loadVerifiedModuleResult") ||
        !coreCompletion() ||
        !stringIn(object, "moduleKind", {"app", "shared", "page"}) ||
        !stringField(object, "moduleId") ||
        !stringIn(object, "status", {"loaded", "failed"})) {
      return false;
    }
    const bool page = *stringField(object, "moduleKind") == "page";
    const bool failed = *stringField(object, "status") == "failed";
    return (page ? validSurface(object, "surfaceId")
                 : field(object, "surfaceId") == nullptr) &&
           validOptionalError(object, failed);
  }
  case CoreMessageKind::CompleteVmInitialization: {
    if (!exactFields(object,
                     {"schemaVersion", "kind", "requestId", "scope",
                      "status"},
                     {"surfaceId", "failedPhase", "error"}) ||
        !validSchemaAndKind(object, "vmInitializationResult") ||
        !coreCompletion() || !stringIn(object, "scope", {"app", "page"}) ||
        !stringIn(object, "status", {"completed", "failed"})) {
      return false;
    }
    const bool page = *stringField(object, "scope") == "page";
    const bool failed = *stringField(object, "status") == "failed";
    if ((page ? !validSurface(object, "surfaceId")
              : field(object, "surfaceId") != nullptr) ||
        !validOptionalError(object, failed)) {
      return false;
    }
    if (!failed) {
      return field(object, "failedPhase") == nullptr;
    }
    return page ? stringIn(object, "failedPhase",
                           {"onInit", "initialEvaluation", "onReady"})
                : stringIn(object, "failedPhase", {"onCreate"});
  }
  case CoreMessageKind::SubmitRenderTransaction: {
    const auto *transactionId = stringField(object, "transactionId");
    const auto *operations = arrayField(object, "operations");
    const auto *causalRequest = stringField(object, "requestId");
    return exactFields(object,
                       {"schemaVersion", "surfaceId", "revision",
                        "transactionId", "operations"},
                       {"requestId"}) &&
           validSchemaAndKind(object, "", false) &&
           validSurface(object, "surfaceId") && transactionId &&
           hasPrefix(*transactionId, "txn:") &&
           isInteger(field(object, "revision"), 1) && operations &&
           std::ranges::all_of(*operations, validRenderOperation) &&
           (!causalRequest || hasPrefix(*causalRequest, "req:"));
  }
  case CoreMessageKind::RegisterHandler: {
    const auto *owner = stringField(object, "ownerInstanceId");
    const auto *handler = stringField(object, "handlerId");
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId", "surfaceId",
                        "ownerInstanceId", "templateHandlerId", "handlerId"}) &&
           validSchemaAndKind(object, "registerHandler") && jsRequest() &&
           validSurface(object, "surfaceId") && owner &&
           (hasPrefix(*owner, "cmp:") || hasPrefix(*owner, "blk:")) &&
           isInteger(field(object, "templateHandlerId"), 1) && handler &&
           hasPrefix(*handler, "hdl:");
  }
  case CoreMessageKind::UnregisterHandler: {
    const auto *handler = stringField(object, "handlerId");
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId", "surfaceId",
                        "handlerId"}) &&
           validSchemaAndKind(object, "unregisterHandler") && jsRequest() &&
           validSurface(object, "surfaceId") && handler &&
           hasPrefix(*handler, "hdl:");
  }
  case CoreMessageKind::NavigationPush:
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId",
                        "sourceSurfaceId", "uri", "params"}) &&
           validSchemaAndKind(object, "navigationPush") && jsRequest() &&
           validSurface(object, "sourceSurfaceId") &&
           stringField(object, "uri") &&
           hasPrefix(*stringField(object, "uri"), "/") &&
           objectField(object, "params");
  case CoreMessageKind::NavigationClose:
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId",
                        "sourceSurfaceId"}) &&
           validSchemaAndKind(object, "navigationClose") && jsRequest() &&
           validSurface(object, "sourceSurfaceId");
  case CoreMessageKind::ShowToast:
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId", "surfaceId",
                        "message", "durationMs"}) &&
           validSchemaAndKind(object, "showToast") && jsRequest() &&
           validSurface(object, "surfaceId") &&
           stringField(object, "message") &&
           !stringField(object, "message")->empty() &&
           isInteger(field(object, "durationMs"), 0);
  case CoreMessageKind::DeviceGetInfo:
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId", "surfaceId"}) &&
           validSchemaAndKind(object, "deviceGetInfo") && jsRequest() &&
           validSurface(object, "surfaceId");
  case CoreMessageKind::SetTitleBar:
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId", "surfaceId",
                        "text"}) &&
           validSchemaAndKind(object, "setTitleBar") && jsRequest() &&
           validSurface(object, "surfaceId") && stringField(object, "text");
  case CoreMessageKind::SetMeta:
    return exactFields(object,
                       {"schemaVersion", "kind", "requestId", "surfaceId"},
                       {"title", "description"}) &&
           validSchemaAndKind(object, "setMeta") && jsRequest() &&
           validSurface(object, "surfaceId") &&
           (stringField(object, "title") ||
            stringField(object, "description"));
  case CoreMessageKind::CompleteLifecycle: {
    if (!exactFields(object,
                     {"schemaVersion", "kind", "requestId", "scope", "hook",
                      "sequence", "status"},
                     {"surfaceId", "error"}) ||
        !validSchemaAndKind(object, "lifecycleResult") ||
        !coreCompletion() || !stringIn(object, "scope", {"app", "page"}) ||
        !stringIn(object, "hook", {"onShow", "onHide", "onDestroy"}) ||
        !isInteger(field(object, "sequence"), 1) ||
        !stringIn(object, "status", {"completed", "failed"})) {
      return false;
    }
    const bool page = *stringField(object, "scope") == "page";
    const bool failed = *stringField(object, "status") == "failed";
    return (page ? validSurface(object, "surfaceId")
                 : field(object, "surfaceId") == nullptr) &&
           validOptionalError(object, failed);
  }
  }
  return false;
}

std::string text(const RuntimeValue::Object &object, std::string_view name) {
  return *stringField(object, name);
}

std::uint64_t integer(const RuntimeValue::Object &object,
                      std::string_view name) {
  return static_cast<std::uint64_t>(
      std::get<double>(field(object, name)->storage()));
}

std::optional<std::string> optionalText(const RuntimeValue::Object &object,
                                        std::string_view name) {
  const auto *value = stringField(object, name);
  return value ? std::optional<std::string>(*value) : std::nullopt;
}

MessageRuntimeError decodeError(const RuntimeValue::Object &object) {
  return {text(object, "code"),
          text(object, "message"),
          std::get<bool>(field(object, "retryable")->storage()),
          optionalText(object, "surfaceId"),
          optionalText(object, "requestId"),
          optionalText(object, "transactionId"),
          optionalText(object, "mountAttemptId")};
}

std::optional<MessageRuntimeError>
optionalError(const RuntimeValue::Object &object) {
  const auto *value = objectField(object, "error");
  return value ? std::optional<MessageRuntimeError>(decodeError(*value))
               : std::nullopt;
}

LogicalNodeRef decodeLogicalNode(const RuntimeValue::Object &object) {
  return {text(object, "ownerInstanceId"), integer(object, "templateNodeId")};
}

HandlerBinding decodeHandler(const RuntimeValue::Object &object) {
  return {text(object, "ownerInstanceId"),
          integer(object, "templateHandlerId"), text(object, "handlerId")};
}

BindingValues decodeBindings(const RuntimeValue::Object &object) {
  BindingValues result;
  for (const auto &[id, value] : object) {
    const auto numericId = static_cast<std::uint64_t>(std::stoull(id));
    if (const auto *stringValue = std::get_if<std::string>(&value.storage())) {
      result.emplace(numericId, *stringValue);
    } else {
      result.emplace(numericId, std::get<bool>(value.storage()));
    }
  }
  return result;
}

InstantiateBlockOperation decodeBlock(const RuntimeValue::Object &object) {
  InstantiateBlockOperation result;
  result.templateBlockId = integer(object, "templateBlockId");
  result.blockInstanceId = text(object, "blockInstanceId");
  result.parent = decodeLogicalNode(*objectField(object, "parent"));
  result.index = integer(object, "index");
  if (const auto *key = field(object, "key")) {
    if (const auto *stringKey = std::get_if<std::string>(&key->storage())) {
      result.key = *stringKey;
    } else {
      result.key = std::get<double>(key->storage());
    }
  }
  result.initialBindings = decodeBindings(*objectField(object, "initialBindings"));
  for (const auto &handler : *arrayField(object, "handlers")) {
    result.handlers.push_back(
        decodeHandler(std::get<RuntimeValue::Object>(handler.storage())));
  }
  return result;
}

RenderOperation decodeRenderOperation(const RuntimeValue &value) {
  const auto &object = std::get<RuntimeValue::Object>(value.storage());
  const auto operationKind = text(object, "kind");
  if (operationKind == "instantiateBlock") {
    return decodeBlock(object);
  }
  if (operationKind == "removeBlock") {
    return RemoveBlockOperation{text(object, "blockInstanceId")};
  }
  if (operationKind == "moveBlock") {
    return MoveBlockOperation{text(object, "blockInstanceId"),
                              decodeLogicalNode(*objectField(object, "parent")),
                              integer(object, "index")};
  }
  BindingValue binding;
  const auto *valueField = field(object, "value");
  if (const auto *stringValue =
          std::get_if<std::string>(&valueField->storage())) {
    binding = *stringValue;
  } else {
    binding = std::get<bool>(valueField->storage());
  }
  return UpdateBindingOperation{text(object, "ownerInstanceId"),
                                integer(object, "templateBindingId"),
                                std::move(binding)};
}

std::vector<InstantiateBlockOperation>
decodeBlocks(const RuntimeValue::Array &array) {
  std::vector<InstantiateBlockOperation> result;
  result.reserve(array.size());
  for (const auto &value : array) {
    result.push_back(
        decodeBlock(std::get<RuntimeValue::Object>(value.storage())));
  }
  return result;
}

std::vector<RenderOperation> decodeOperations(const RuntimeValue::Array &array) {
  std::vector<RenderOperation> result;
  result.reserve(array.size());
  for (const auto &value : array) {
    result.push_back(decodeRenderOperation(value));
  }
  return result;
}

std::vector<HandlerBinding> decodeHandlers(const RuntimeValue::Array &array) {
  std::vector<HandlerBinding> result;
  result.reserve(array.size());
  for (const auto &value : array) {
    result.push_back(
        decodeHandler(std::get<RuntimeValue::Object>(value.storage())));
  }
  return result;
}

bool validTypedError(const std::optional<MessageRuntimeError> &error,
                     bool required) {
  if (required != error.has_value()) {
    return false;
  }
  if (!error) {
    return true;
  }
  RuntimeValue::Object encoded{{"code", error->code},
                               {"message", error->message},
                               {"retryable", error->retryable}};
  if (error->surfaceId) encoded.emplace("surfaceId", *error->surfaceId);
  if (error->requestId) encoded.emplace("requestId", *error->requestId);
  if (error->transactionId)
    encoded.emplace("transactionId", *error->transactionId);
  if (error->mountAttemptId)
    encoded.emplace("mountAttemptId", *error->mountAttemptId);
  return validErrorObject(encoded);
}

bool validLogicalNode(const LogicalNodeRef &node) {
  return (hasPrefix(node.ownerInstanceId, "cmp:") ||
          hasPrefix(node.ownerInstanceId, "blk:")) &&
         node.templateNodeId > 0;
}

bool validDynamicValues(const DynamicValues &values, const ValueLimits &limits) {
  return validateRuntimeValue(RuntimeValue(values), limits).ok();
}

bool withinNodeBudget(std::size_t nodes, const ValueLimits &limits) {
  return nodes <= limits.maxNodes;
}

bool validCallback(const JsInboundMessage &message, const ValueLimits &limits) {
  return std::visit(
      [&](const auto &typed) -> bool {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, LoadVerifiedModule>) {
          const bool page = typed.moduleKind == "page";
          const bool app = typed.moduleKind == "app";
          const auto expectedCount =
              (typed.expectedBindingIds ? typed.expectedBindingIds->size() : 0) +
              (typed.expectedHandlerIds ? typed.expectedHandlerIds->size() : 0);
          return validCoreRequestId(typed.requestId) && !typed.packageId.empty() &&
                 (app || typed.moduleKind == "shared" || page) &&
                 !typed.moduleId.empty() &&
                 typed.cacheScope == (page ? "surface" : "appRuntime") &&
                 (page == typed.surfaceId.has_value()) &&
                 !typed.bundle.path.empty() && typed.bundle.byteLength > 0 &&
                 typed.bundle.sha256.size() == 64 &&
                 typed.bundle.bytes && !typed.bundle.bytes->empty() &&
                 typed.bundle.byteLength == typed.bundle.bytes->size() &&
                 (page ? typed.expectedBootstrap.has_value() &&
                             typed.expectedBindingIds.has_value() &&
                             typed.expectedHandlerIds.has_value()
                       : !typed.expectedBindingIds && !typed.expectedHandlerIds) &&
                 (app ? typed.expectedBootstrap.has_value()
                      : page || !typed.expectedBootstrap) &&
                 withinNodeBudget(typed.dependencies.size() + expectedCount,
                                  limits);
        } else if constexpr (std::is_same_v<T, AppContext>) {
          return !typed.packageId.empty() && !typed.versionName.empty() &&
                 typed.versionCode > 0 && !typed.runtimeVersion.empty() &&
                 withinNodeBudget(typed.declaredCapabilities.size(), limits);
        } else if constexpr (std::is_same_v<T, SurfaceContext>) {
          return hasPrefix(typed.surfaceId, "srf:") &&
                 !typed.packageId.empty() && hasPrefix(typed.route, "/") &&
                 !typed.templateId.empty() && typed.viewport.width > 0 &&
                 typed.viewport.height > 0 &&
                 typed.viewport.unit == "logical-px" &&
                 validDynamicValues(typed.params, limits) &&
                 withinNodeBudget(typed.hostCapabilities.size(), limits);
        } else if constexpr (std::is_same_v<T, VmInitializationDispatch>) {
          const bool page = typed.scope == "page";
          return validCoreRequestId(typed.requestId) &&
                 (page || typed.scope == "app") &&
                 (page ? typed.surfaceId && hasPrefix(*typed.surfaceId, "srf:")
                       : !typed.surfaceId);
        } else if constexpr (std::is_same_v<T, LifecycleDispatch>) {
          const bool page = typed.scope == "page";
          return validCoreRequestId(typed.requestId) &&
                 (page || typed.scope == "app") &&
                 (typed.hook == "onShow" || typed.hook == "onHide" ||
                  typed.hook == "onDestroy") &&
                 typed.sequence > 0 &&
                 (page ? typed.surfaceId && hasPrefix(*typed.surfaceId, "srf:")
                       : !typed.surfaceId);
        } else if constexpr (std::is_same_v<T, JsEventDispatch>) {
          return validPlatformRequestId(typed.requestId) &&
                 hasPrefix(typed.surfaceId, "srf:") &&
                 validLogicalNode(typed.target) &&
                 validLogicalNode(typed.currentTarget) &&
                 hasPrefix(typed.handlerId, "hdl:") &&
                 typed.eventType == "click" &&
                 (typed.phase == "target" || typed.phase == "bubble") &&
                 std::isfinite(typed.timestamp) && typed.timestamp >= 0 &&
                 validDynamicValues(typed.payload, limits);
        } else if constexpr (std::is_same_v<T, InstantiateTemplateResult>) {
          const bool failed = typed.status == "failed";
          return validJsRequestId(typed.requestId) &&
                 hasPrefix(typed.surfaceId, "srf:") &&
                 (failed || typed.status == "presented") &&
                 validTypedError(typed.error, failed) &&
                 (failed ? !typed.committedRevision
                         : typed.committedRevision == 0);
        } else if constexpr (std::is_same_v<T, HandlerRegistrationResult>) {
          const bool failed = typed.status == "failed";
          const bool matching =
              (typed.operation == "register" && typed.status == "registered") ||
              (typed.operation == "unregister" &&
               typed.status == "unregistered") || failed;
          return validJsRequestId(typed.requestId) && matching &&
                 hasPrefix(typed.surfaceId, "srf:") &&
                 hasPrefix(typed.handlerId, "hdl:") &&
                 validTypedError(typed.error, failed);
        } else if constexpr (std::is_same_v<T, RenderTransactionResult>) {
          const bool failed = typed.status != "presented";
          return hasPrefix(typed.surfaceId, "srf:") &&
                 hasPrefix(typed.transactionId, "txn:") &&
                 typed.submittedRevision > 0 &&
                 (typed.status == "presented" || typed.status == "rejected" ||
                  typed.status == "cancelled" ||
                  typed.status == "presentationFailed") &&
                 validTypedError(typed.error, failed);
        } else if constexpr (std::is_same_v<T, NavigationPushResult>) {
          const bool failed = typed.status == "failed";
          return validJsRequestId(typed.requestId) &&
                 hasPrefix(typed.sourceSurfaceId, "srf:") &&
                 (failed || typed.status == "presented") &&
                 validTypedError(typed.error, failed) &&
                 (failed ? !typed.targetSurfaceId
                         : typed.targetSurfaceId &&
                               hasPrefix(*typed.targetSurfaceId, "srf:"));
        } else if constexpr (std::is_same_v<T, NavigationCloseResult>) {
          const bool failed = typed.status == "failed";
          return validJsRequestId(typed.requestId) &&
                 hasPrefix(typed.sourceSurfaceId, "srf:") &&
                 (failed || typed.status == "closed") &&
                 validTypedError(typed.error, failed) &&
                 (failed ? !typed.revealedSurfaceId
                         : typed.revealedSurfaceId &&
                               hasPrefix(*typed.revealedSurfaceId, "srf:"));
        } else if constexpr (std::is_same_v<T, DeviceGetInfoResult>) {
          const bool failed = typed.status == "failed";
          return validJsRequestId(typed.requestId) &&
                 hasPrefix(typed.surfaceId, "srf:") &&
                 (failed || typed.status == "completed") &&
                 validTypedError(typed.error, failed) &&
                 (failed ? !typed.info : typed.info.has_value());
        } else if constexpr (std::is_same_v<T, ShowToastResult> ||
                             std::is_same_v<T, SetTitleBarResult> ||
                             std::is_same_v<T, SetMetaResult>) {
          const bool failed = typed.status == "failed";
          return validJsRequestId(typed.requestId) &&
                 hasPrefix(typed.surfaceId, "srf:") &&
                 (failed || typed.status == "completed") &&
                 validTypedError(typed.error, failed);
        } else if constexpr (std::is_same_v<T, SurfaceStatusChanged>) {
          static const std::set<std::string, std::less<>> lifecycle{
              "creating", "awaitingTemplate", "mounting", "presenting",
              "visible", "hidden", "destroying", "destroyed"};
          return hasPrefix(typed.surfaceId, "srf:") &&
                 lifecycle.contains(typed.lifecycleState) &&
                 (typed.healthState == "normal" ||
                  typed.healthState == "degraded" ||
                  typed.healthState == "failed");
        }
      },
      message);
}

} // namespace

DecodeCoreResult decodeCoreMessage(CoreMessageKind expectedKind,
                                   const RuntimeValue &value,
                                   const ValueLimits &limits) noexcept {
  try {
    if (!validateRuntimeValue(value, limits).ok()) {
      return DecodeCoreResult::failure(invalid("RuntimeValue limits exceeded"));
    }
    const auto *object = std::get_if<RuntimeValue::Object>(&value.storage());
    if (!object) {
      return DecodeCoreResult::failure(invalid("message must be an object"));
    }
    const auto *version = field(*object, "schemaVersion");
    if (!isInteger(version, 1) ||
        std::get<double>(version->storage()) != 1.0) {
      return DecodeCoreResult::failure(unsupported());
    }
    if (!validateCoreObject(expectedKind, *object)) {
      return DecodeCoreResult::failure(invalid("message does not match schema"));
    }
    switch (expectedKind) {
    case CoreMessageKind::InstantiateTemplate:
      return DecodeCoreResult::success(InstantiateTemplate{
          text(*object, "requestId"), text(*object, "surfaceId"),
          text(*object, "templateId"), text(*object, "ownerInstanceId"),
          decodeBindings(*objectField(*object, "initialBindings")),
          decodeBlocks(*arrayField(*object, "initialBlocks")),
          decodeHandlers(*arrayField(*object, "initialHandlers"))});
    case CoreMessageKind::CompleteVerifiedModuleLoad:
      return DecodeCoreResult::success(CompleteVerifiedModuleLoad{
          text(*object, "requestId"), text(*object, "moduleKind"),
          text(*object, "moduleId"), text(*object, "status"),
          optionalText(*object, "surfaceId"), optionalError(*object)});
    case CoreMessageKind::CompleteVmInitialization:
      return DecodeCoreResult::success(CompleteVmInitialization{
          text(*object, "requestId"), text(*object, "scope"),
          text(*object, "status"), optionalText(*object, "surfaceId"),
          optionalText(*object, "failedPhase"), optionalError(*object)});
    case CoreMessageKind::SubmitRenderTransaction:
      return DecodeCoreResult::success(SubmitRenderTransaction{
          text(*object, "surfaceId"), text(*object, "transactionId"),
          integer(*object, "revision"), optionalText(*object, "requestId"),
          decodeOperations(*arrayField(*object, "operations"))});
    case CoreMessageKind::RegisterHandler:
      return DecodeCoreResult::success(RegisterHandler{
          text(*object, "requestId"), text(*object, "surfaceId"),
          text(*object, "ownerInstanceId"), text(*object, "handlerId"),
          integer(*object, "templateHandlerId")});
    case CoreMessageKind::UnregisterHandler:
      return DecodeCoreResult::success(UnregisterHandler{
          text(*object, "requestId"), text(*object, "surfaceId"),
          text(*object, "handlerId")});
    case CoreMessageKind::NavigationPush:
      return DecodeCoreResult::success(NavigationPush{
          text(*object, "requestId"), text(*object, "sourceSurfaceId"),
          text(*object, "uri"), *objectField(*object, "params")});
    case CoreMessageKind::NavigationClose:
      return DecodeCoreResult::success(NavigationClose{
          text(*object, "requestId"), text(*object, "sourceSurfaceId")});
    case CoreMessageKind::ShowToast:
      return DecodeCoreResult::success(ShowToast{
          text(*object, "requestId"), text(*object, "surfaceId"),
          text(*object, "message"), integer(*object, "durationMs")});
    case CoreMessageKind::DeviceGetInfo:
      return DecodeCoreResult::success(DeviceGetInfo{
          text(*object, "requestId"), text(*object, "surfaceId")});
    case CoreMessageKind::SetTitleBar:
      return DecodeCoreResult::success(SetTitleBar{
          text(*object, "requestId"), text(*object, "surfaceId"),
          text(*object, "text")});
    case CoreMessageKind::SetMeta:
      return DecodeCoreResult::success(SetMeta{
          text(*object, "requestId"), text(*object, "surfaceId"),
          optionalText(*object, "title"), optionalText(*object, "description")});
    case CoreMessageKind::CompleteLifecycle:
      return DecodeCoreResult::success(CompleteLifecycle{
          text(*object, "requestId"), text(*object, "scope"),
          text(*object, "hook"), text(*object, "status"),
          integer(*object, "sequence"), optionalText(*object, "surfaceId"),
          optionalError(*object)});
    }
  } catch (const std::bad_alloc &) {
    return DecodeCoreResult::failure(
        {AbiErrorCode::OutOfMemory, "out of memory", true, std::nullopt,
         std::nullopt, std::nullopt, std::nullopt});
  } catch (...) {
    return DecodeCoreResult::failure(invalid("message decoding failed"));
  }
  return DecodeCoreResult::failure(invalid("unknown message kind"));
}

ValidateCallbackResult
validateJsInboundMessage(const JsInboundMessage &message,
                         const ValueLimits &limits) noexcept {
  try {
    if (!validCallback(message, limits)) {
      return ValidateCallbackResult::failure(
          invalid("callback does not match schema"));
    }
    return ValidateCallbackResult::success();
  } catch (const std::bad_alloc &) {
    return ValidateCallbackResult::failure(
        {AbiErrorCode::OutOfMemory, "out of memory", true, std::nullopt,
         std::nullopt, std::nullopt, std::nullopt});
  } catch (...) {
    return ValidateCallbackResult::failure(
        invalid("callback validation failed"));
  }
}

RuntimeValue encodeEnqueueResult(const EnqueueResult &result) {
  RuntimeValue::Object encoded{{"ok", RuntimeValue(result.ok)}};
  if (!result.ok && result.error) {
    RuntimeValue::Object error{
        {"code", RuntimeValue(std::string(abiErrorCodeName(result.error->code)))},
        {"message", RuntimeValue(result.error->message)},
        {"retryable", RuntimeValue(result.error->retryable)},
    };
    const auto add = [&](std::string name,
                         const std::optional<std::string> &value) {
      if (value) {
        error.emplace(std::move(name), RuntimeValue(*value));
      }
    };
    add("surfaceId", result.error->surfaceId);
    add("requestId", result.error->requestId);
    add("transactionId", result.error->transactionId);
    add("mountAttemptId", result.error->mountAttemptId);
    encoded.emplace("error", RuntimeValue(std::move(error)));
  }
  return RuntimeValue(std::move(encoded));
}

std::optional<CorrelationKey>
coreCorrelation(const CoreInboundMessage &message) {
  return std::visit(
      [](const auto &typed) -> std::optional<CorrelationKey> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, SubmitRenderTransaction>) {
          return CorrelationKey{CorrelationKeyKind::Transaction,
                                typed.transactionId};
        } else {
          return CorrelationKey{CorrelationKeyKind::Request, typed.requestId};
        }
      },
      message);
}

std::optional<std::string>
coreSurfaceId(const CoreInboundMessage &message) {
  return std::visit(
      [](const auto &typed) -> std::optional<std::string> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, NavigationPush> ||
                      std::is_same_v<T, NavigationClose>) {
          return typed.sourceSurfaceId;
        } else if constexpr (std::is_same_v<T, CompleteVerifiedModuleLoad> ||
                             std::is_same_v<T, CompleteVmInitialization> ||
                             std::is_same_v<T, CompleteLifecycle>) {
          return typed.surfaceId;
        } else {
          return typed.surfaceId;
        }
      },
      message);
}

std::optional<JsCallbackKind>
expectedResultKind(const CoreInboundMessage &message) {
  switch (messageKind(message)) {
  case CoreMessageKind::InstantiateTemplate:
    return JsCallbackKind::InstantiateTemplateResult;
  case CoreMessageKind::SubmitRenderTransaction:
    return JsCallbackKind::RenderTransactionResult;
  case CoreMessageKind::RegisterHandler:
  case CoreMessageKind::UnregisterHandler:
    return JsCallbackKind::HandlerRegistrationResult;
  case CoreMessageKind::NavigationPush:
    return JsCallbackKind::NavigationPushResult;
  case CoreMessageKind::NavigationClose:
    return JsCallbackKind::NavigationCloseResult;
  case CoreMessageKind::ShowToast:
    return JsCallbackKind::ShowToastResult;
  case CoreMessageKind::DeviceGetInfo:
    return JsCallbackKind::DeviceGetInfoResult;
  case CoreMessageKind::SetTitleBar:
    return JsCallbackKind::SetTitleBarResult;
  case CoreMessageKind::SetMeta:
    return JsCallbackKind::SetMetaResult;
  case CoreMessageKind::CompleteVerifiedModuleLoad:
  case CoreMessageKind::CompleteVmInitialization:
  case CoreMessageKind::CompleteLifecycle:
    return std::nullopt;
  }
  return std::nullopt;
}

bool coreMessageNeedsCorrelation(const CoreInboundMessage &message) {
  return expectedResultKind(message).has_value();
}

std::optional<CorrelationKey>
callbackCorrelation(const JsInboundMessage &message) {
  return std::visit(
      [](const auto &typed) -> std::optional<CorrelationKey> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, RenderTransactionResult>) {
          return CorrelationKey{CorrelationKeyKind::Transaction,
                                typed.transactionId};
        } else if constexpr (std::is_same_v<T, AppContext> ||
                             std::is_same_v<T, SurfaceContext> ||
                             std::is_same_v<T, SurfaceStatusChanged>) {
          return std::nullopt;
        } else {
          return CorrelationKey{CorrelationKeyKind::Request, typed.requestId};
        }
      },
      message);
}

std::optional<std::string>
callbackSurfaceId(const JsInboundMessage &message) {
  return std::visit(
      [](const auto &typed) -> std::optional<std::string> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, AppContext> ||
                      std::is_same_v<T, LoadVerifiedModule>) {
          if constexpr (std::is_same_v<T, LoadVerifiedModule>) {
            return typed.surfaceId;
          }
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, VmInitializationDispatch> ||
                             std::is_same_v<T, LifecycleDispatch>) {
          return typed.surfaceId;
        } else if constexpr (std::is_same_v<T, NavigationPushResult> ||
                             std::is_same_v<T, NavigationCloseResult>) {
          return typed.sourceSurfaceId;
        } else {
          return typed.surfaceId;
        }
      },
      message);
}

bool callbackIsResult(const JsInboundMessage &message) {
  switch (callbackKind(message)) {
  case JsCallbackKind::InstantiateTemplateResult:
  case JsCallbackKind::HandlerRegistrationResult:
  case JsCallbackKind::RenderTransactionResult:
  case JsCallbackKind::NavigationPushResult:
  case JsCallbackKind::NavigationCloseResult:
  case JsCallbackKind::ShowToastResult:
  case JsCallbackKind::DeviceGetInfoResult:
  case JsCallbackKind::SetTitleBarResult:
  case JsCallbackKind::SetMetaResult:
    return true;
  case JsCallbackKind::LoadVerifiedModule:
  case JsCallbackKind::AppContext:
  case JsCallbackKind::SurfaceContext:
  case JsCallbackKind::VmInitializationDispatch:
  case JsCallbackKind::LifecycleDispatch:
  case JsCallbackKind::JsEventDispatch:
  case JsCallbackKind::SurfaceStatusChanged:
    return false;
  }
  return false;
}

} // namespace quickapp::js::abi
