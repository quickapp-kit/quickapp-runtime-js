#include "quickapp/js/module/module_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include "engine/internal/ref_access.h"

namespace quickapp::js::module {
namespace {

struct Sha256 {
  std::array<std::uint32_t, 8> state{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> block{};
  std::size_t used{0};
  std::uint64_t total{0};

  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  static std::uint32_t rotr(std::uint32_t x, unsigned n) {
    return (x >> n) | (x << (32U - n));
  }
  static std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (~x & z);
  }
  static std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
  }
  static std::uint32_t big0(std::uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
  }
  static std::uint32_t big1(std::uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
  }
  static std::uint32_t small0(std::uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
  }
  static std::uint32_t small1(std::uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
  }

  void transform(const std::uint8_t *data) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24U) |
             (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16U) |
             (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8U) |
             static_cast<std::uint32_t>(data[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      w[i] = small1(w[i - 2]) + w[i - 7] + small0(w[i - 15]) + w[i - 16];
    }
    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t i = 0; i < 64; ++i) {
      const auto t1 = h + big1(e) + ch(e, f, g) + k[i] + w[i];
      const auto t2 = big0(a) + maj(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  void update(std::span<const std::uint8_t> bytes) {
    total += bytes.size();
    for (const auto byte : bytes) {
      block[used++] = byte;
      if (used == block.size()) {
        transform(block.data());
        used = 0;
      }
    }
  }

  std::string finish() {
    const auto bitLength = total * 8U;
    block[used++] = 0x80;
    if (used > 56) {
      while (used < 64) block[used++] = 0;
      transform(block.data());
      used = 0;
    }
    while (used < 56) block[used++] = 0;
    for (int i = 7; i >= 0; --i) {
      block[used++] = static_cast<std::uint8_t>(bitLength >> (i * 8));
    }
    transform(block.data());
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto word : state) {
      for (int i = 7; i >= 0; --i) {
        result.push_back(hex[(word >> (i * 4)) & 0xfU]);
      }
    }
    return result;
  }
};

bool validUtf8(std::span<const std::uint8_t> bytes) noexcept {
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto first = bytes[i++];
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if (first <= 0x7f) continue;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1;
      codepoint = first & 0x1fU;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2;
      codepoint = first & 0x0fU;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (i + continuation > bytes.size()) return false;
    for (std::size_t j = 0; j < continuation; ++j) {
      const auto next = bytes[i++];
      if ((next & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3fU);
    }
    if ((continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  }
  return true;
}

std::string sha256(std::span<const std::uint8_t> bytes) {
  Sha256 digest;
  digest.update(bytes);
  return digest.finish();
}

ModuleError error(ModuleErrorCode code, std::string message,
                  bool retryable = false) {
  return {code, std::move(message), retryable};
}

bool prefix(std::string_view value, std::string_view expected) {
  return value.starts_with(expected);
}

const RuntimeValue *objectField(const RuntimeValue::Object &object,
                                std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

const std::string *stringField(const RuntimeValue::Object &object,
                               std::string_view name) {
  const auto *value = objectField(object, name);
  return value ? std::get_if<std::string>(&value->storage()) : nullptr;
}

bool exactFields(const RuntimeValue::Object &object,
                 std::initializer_list<std::string_view> required,
                 std::initializer_list<std::string_view> optional = {}) {
  std::set<std::string_view> allowed(required.begin(), required.end());
  allowed.insert(optional.begin(), optional.end());
  for (const auto key : required) if (!object.contains(key)) return false;
  return std::all_of(object.begin(), object.end(), [&](const auto &entry) {
    return allowed.contains(entry.first);
  });
}

bool positiveDecimal(std::string_view text, std::uint64_t &value) noexcept {
  if (text.empty() || text.front() == '0') return false;
  std::uint64_t parsed = 0;
  for (const auto character : text) {
    if (character < '0' || character > '9') return false;
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
      return false;
    parsed = parsed * 10U + digit;
  }
  if (parsed == 0) return false;
  value = parsed;
  return true;
}

bool validRequestId(std::string_view value) noexcept {
  constexpr std::string_view prefix = "req:j-";
  if (!value.starts_with(prefix)) return false;
  std::uint64_t number = 0;
  return positiveDecimal(value.substr(prefix.size()), number);
}

} // namespace

struct ModuleLoader::EvaluationContext {
  std::string moduleId;
  std::vector<std::string> dependencies;
  std::optional<ModuleError> failure;
};

struct ModuleLoader::Transaction final : EvaluationContext {
  std::string cacheKey;
  std::string moduleKind;
  std::string packageId;
  std::string surfaceId;
  std::uint64_t surfaceGeneration{0};
  std::size_t sourceBytes{0};
  std::uint32_t defineCount{0};
  std::uint32_t bootstrapCount{0};
  std::string bootstrapKind;
  std::string bootstrapModuleId;
  std::optional<std::string> bootstrapTemplateId;
  JsValueRef factory;
};

struct ModuleLoader::Entry {
  ModuleDefinitionHandle handle;
  std::string cacheKey;
  std::string packageId;
  std::string moduleKind;
  std::string moduleId;
  std::vector<std::string> dependencies;
  std::string sha256;
  std::optional<std::string> templateId;
  enum class State { Defined, Evaluating, Loaded, Failed } state{State::Defined};
  std::optional<ModuleError> failure;
  JsValueRef factory;
  JsValueRef exports;
  JsValueRef createVm;
  JsValueRef bindingEvaluators;
  std::vector<std::uint64_t> bindingIds;
  std::map<std::uint64_t, std::string> handlerMethods;
  std::set<std::string, std::less<>> leases;
};

struct ModuleLoader::SurfaceScope {
  std::uint64_t generation{1};
  bool open{true};
};

std::string_view moduleErrorCodeName(ModuleErrorCode code) noexcept {
  switch (code) {
  case ModuleErrorCode::InvalidArgument: return "ABI_INVALID_ARGUMENT";
  case ModuleErrorCode::PackageIntegrityFailed: return "PACKAGE_INTEGRITY_FAILED";
  case ModuleErrorCode::ModuleAbiUnsupported: return "MODULE_ABI_UNSUPPORTED";
  case ModuleErrorCode::JsException: return "JS_EXCEPTION";
  case ModuleErrorCode::OutOfMemory: return "OUT_OF_MEMORY";
  case ModuleErrorCode::QueueOverflow: return "QUEUE_OVERFLOW";
  case ModuleErrorCode::SurfaceNotFound: return "SURFACE_NOT_FOUND";
  case ModuleErrorCode::PortClosed: return "PORT_CLOSED";
  }
  return "MODULE_ABI_UNSUPPORTED";
}

ModuleLoader::ModuleLoader(JsEngineService &engineService,
                           ModuleCompletionPort &completion,
                           std::string appRuntimeId, std::string packageId,
                           ModuleLoaderLimits limits,
                           FrameworkModuleResolverPort *frameworkModules)
    : engineService_(engineService), completion_(completion),
      appRuntimeId_(std::move(appRuntimeId)), packageId_(std::move(packageId)),
      limits_(limits), frameworkModules_(frameworkModules) {}

ModuleLoader::~ModuleLoader() {
  if (running_) std::terminate();
}

bool ModuleLoader::onExecutor() const noexcept {
  return engineService_.executor().isOnExecutor();
}

abi::CallbackSlots ModuleLoader::callbackSlots() noexcept {
  abi::CallbackSlots slots;
  slots.loadVerifiedModule = [this](const abi::LoadVerifiedModule &message) {
    onLoadVerifiedModule(message);
  };
  return slots;
}

NativeFunctionResult ModuleLoader::nullValue() noexcept {
  if (!engine_ || !context_) {
    return NativeFunctionResult::failure(
        {RuntimeErrorCode::JsException, "module loader is stopped"});
  }
  auto value = engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
  if (!value.ok()) {
    return NativeFunctionResult::failure(
        {value.error().kind == EngineExceptionKind::OutOfMemory
             ? RuntimeErrorCode::OutOfMemory
             : RuntimeErrorCode::JsException,
         value.error().message});
  }
  return NativeFunctionResult::success(std::move(value).value());
}

bool ModuleLoader::startOnExecutor(JsEnginePort &engine,
                                   const JsContextRef &context) noexcept {
  if (!onExecutor() || running_) return false;
  engine_ = &engine;
  context_ = &context;
  try {
    static constexpr std::string_view validatorSource = R"JS(
(function() {
  function data(o, k) {
    const d = Object.getOwnPropertyDescriptor(o, k);
    return !!d && Object.prototype.hasOwnProperty.call(d, "value") &&
      !Object.prototype.hasOwnProperty.call(d, "get") &&
      !Object.prototype.hasOwnProperty.call(d, "set");
  }
  function plain(o) { return o !== null && typeof o === "object" &&
    Object.getPrototypeOf(o) === Object.prototype; }
  function exact(o, names) {
    if (!plain(o)) return false;
    const keys = Object.getOwnPropertyNames(o);
    if (keys.length !== names.length) return false;
    for (let i = 0; i < names.length; ++i) {
      let found = false;
      for (let j = 0; j < keys.length; ++j) {
        if (keys[j] === names[i]) { found = true; break; }
      }
      if (!found || !data(o, names[i])) return false;
    }
    return true;
  }
  return function(def, kind) {
    if (kind === "app") {
      if (!exact(def, ["schemaVersion", "kind", "createAppVm"]) ||
          def.schemaVersion !== 1 || def.kind !== "app" ||
          typeof def.createAppVm !== "function") return {ok:false};
      return {ok:true, bindingIds:[], handlerIds:[], handlerNames:{}};
    }
    if (kind !== "page" || !exact(def, ["schemaVersion", "kind",
        "createPageVm", "bindingEvaluators", "handlerMethods"]) ||
        def.schemaVersion !== 1 || def.kind !== "page" ||
        typeof def.createPageVm !== "function" ||
        !plain(def.bindingEvaluators) || !plain(def.handlerMethods)) return {ok:false};
    const bindingIds = Object.getOwnPropertyNames(def.bindingEvaluators);
    const handlerIds = Object.getOwnPropertyNames(def.handlerMethods);
    for (const k of bindingIds) {
      if (!data(def.bindingEvaluators, k) ||
          typeof def.bindingEvaluators[k] !== "function") return {ok:false};
    }
    const handlerNames = {};
    for (const k of handlerIds) {
      if (!data(def.handlerMethods, k) ||
          typeof def.handlerMethods[k] !== "string" ||
          def.handlerMethods[k].length === 0) return {ok:false};
      handlerNames[k] = def.handlerMethods[k];
    }
    return {ok:true, bindingIds, handlerIds, handlerNames};
  };
})()
)JS";
    auto validator = engine_->evaluate(
        context, SourceUnit{"js-s03-definition-validator", "quickapp://s03/validator", 
                            std::string(validatorSource), SourceMode::Script});
    if (!validator.ok()) {
      releaseAllOnExecutor();
      return false;
    }
    definitionValidator_ = std::move(validator).value();
    auto validatorCallable = engine_->isCallable(context, definitionValidator_);
    if (!validatorCallable.ok() || !validatorCallable.value()) {
      releaseAllOnExecutor();
      return false;
    }
    const std::array<NativeFunctionSpec, 3> specs{{
        NativeFunctionSpec{"$app_define$", 3, 3,
                           [this](const NativeCallView &call) {
                             return invokeDefine(call);
                           }},
        NativeFunctionSpec{"$app_bootstrap$", 2, 2,
                           [this](const NativeCallView &call) {
                             return invokeBootstrap(call);
                           }},
        NativeFunctionSpec{"$app_require$", 1, 1,
                           [this](const NativeCallView &call) {
                             return invokeRequire(call);
                           }}}};
    for (const auto &spec : specs) {
      auto token = engine_->bindNativeFunction(context, spec);
      if (!token.ok()) {
        for (auto it = bindingTokens_.rbegin(); it != bindingTokens_.rend(); ++it)
          static_cast<void>(engine_->unbindNativeFunction(context, *it));
        bindingTokens_.clear();
        definitionValidator_.reset();
        engine_ = nullptr;
        context_ = nullptr;
        return false;
      }
      bindingTokens_.push_back(std::move(token).value());
    }
    running_ = true;
    return true;
  } catch (...) {
    releaseAllOnExecutor();
    return false;
  }
}

bool ModuleLoader::openSurfaceOnExecutor(std::string surfaceId) noexcept {
  if (!onExecutor() || !running_ || !prefix(surfaceId, "srf:")) return false;
  if (surfaces_.contains(surfaceId)) return false;
  surfaces_.emplace(std::move(surfaceId), SurfaceScope{});
  return true;
}

bool ModuleLoader::closeSurfaceOnExecutor(std::string_view surfaceId) noexcept {
  if (!onExecutor()) return false;
  const auto found = surfaces_.find(surfaceId);
  if (found == surfaces_.end()) return false;
  found->second.open = false;
  ++found->second.generation;
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto &entry = *it->second;
    entry.leases.erase(std::string(surfaceId));
    if (entry.moduleKind == "page" && entry.leases.empty() &&
        entry.state != Entry::State::Loaded) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = outbox_.begin(); it != outbox_.end();) {
    if (it->second.surfaceId && *it->second.surfaceId == surfaceId)
      it = outbox_.erase(it);
    else
      ++it;
  }
  surfaces_.erase(found);
  return true;
}

bool ModuleLoader::validateInput(const abi::LoadVerifiedModule &message,
                                 ModuleError &failure) const noexcept {
  if (!running_ || !message.bundle.bytes || message.bundle.bytes->empty() ||
      message.bundle.byteLength > limits_.maxSourceBytes ||
      message.bundle.sha256.size() != 64 || message.packageId != packageId_ ||
      !validRequestId(message.requestId) || message.moduleId.empty() ||
      message.dependencies.size() > limits_.maxDependencies) {
    failure = error(ModuleErrorCode::InvalidArgument, "invalid verified module input");
    return false;
  }
  if (message.bundle.byteLength != message.bundle.bytes->size()) {
    failure = error(ModuleErrorCode::PackageIntegrityFailed,
                    "verified byte length mismatch");
    return false;
  }
  const bool app = message.moduleKind == "app";
  const bool shared = message.moduleKind == "shared";
  const bool page = message.moduleKind == "page";
  if ((!app && !shared && !page) ||
      message.cacheScope != (page ? "surface" : "appRuntime") ||
      (page != message.surfaceId.has_value()) ||
      (page && (!message.expectedBootstrap || !message.expectedBindingIds ||
                !message.expectedHandlerIds)) ||
      (!page && (message.expectedBindingIds || message.expectedHandlerIds)) ||
      ((app || page) != message.expectedBootstrap.has_value()) ||
      (shared && message.expectedBootstrap)) {
    failure = error(ModuleErrorCode::InvalidArgument, "verified module scope mismatch");
    return false;
  }
  if (page) {
    const auto found = surfaces_.find(*message.surfaceId);
    if (found == surfaces_.end() || !found->second.open) {
      failure = error(ModuleErrorCode::SurfaceNotFound, "Surface is closed", true);
      return false;
    }
    if (message.expectedBindingIds->size() > limits_.maxExpectedIds ||
        message.expectedHandlerIds->size() > limits_.maxExpectedIds) {
      failure = error(ModuleErrorCode::QueueOverflow, "expected ID budget exceeded", true);
      return false;
    }
  }
  return true;
}

bool ModuleLoader::parseDependencies(const RuntimeValue &value,
                                     std::vector<std::string> &dependencies,
                                     ModuleError &failure) const noexcept {
  const auto *array = std::get_if<RuntimeValue::Array>(&value.storage());
  if (!array || array->size() > limits_.maxDependencies) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "dependencies must be a bounded array");
    return false;
  }
  for (const auto &item : *array) {
    const auto *text = std::get_if<std::string>(&item.storage());
    if (!text || text->empty() || std::find(dependencies.begin(), dependencies.end(), *text) != dependencies.end()) {
      failure = error(ModuleErrorCode::ModuleAbiUnsupported, "invalid dependency list");
      return false;
    }
    dependencies.push_back(*text);
  }
  return true;
}

bool ModuleLoader::parseDefine(const NativeCallView &call,
                               Transaction &transaction,
                               ModuleError &failure) noexcept {
  if (call.args.size() != 3) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "define arity mismatch");
    return false;
  }
  auto moduleId = engine_->toRuntimeValue(*context_,
      *detail::RefAccess::viewedValue(call.args[0]), {64, 2048});
  auto dependencies = engine_->toRuntimeValue(*context_,
      *detail::RefAccess::viewedValue(call.args[1]), {64, 2048});
  if (!moduleId.ok() || !dependencies.ok()) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "define arguments are not data");
    return false;
  }
  const auto *id = std::get_if<std::string>(&moduleId.value().storage());
  if (!id || *id != transaction.moduleId || transaction.defineCount != 0) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "define module identity mismatch");
    return false;
  }
  auto callable = engine_->isCallable(
      *context_, *detail::RefAccess::viewedValue(call.args[2]));
  transaction.dependencies.clear();
  if (!parseDependencies(dependencies.value(), transaction.dependencies, failure) ||
      !callable.ok() || !callable.value()) {
    if (!failure.message.size() || callable.ok())
      failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                      "factory is not callable");
    return false;
  }
  auto factory = engine_->retain(*context_, call.args[2]);
  if (!factory.ok()) {
    failure = mapEngineError(factory.error());
    return false;
  }
  transaction.factory = std::move(factory).value();
  transaction.defineCount = 1;
  return true;
}

NativeFunctionResult ModuleLoader::invokeDefine(const NativeCallView &call) noexcept {
  if (!currentTransaction_) return NativeFunctionResult::failure({RuntimeErrorCode::JsException, "define outside load"});
  ModuleError failure = error(ModuleErrorCode::ModuleAbiUnsupported, "define failed");
  if (!parseDefine(call, *currentTransaction_, failure)) {
    currentTransaction_->failure = failure;
    return NativeFunctionResult::failure({RuntimeErrorCode::ModuleAbiUnsupported, failure.message});
  }
  return nullValue();
}

bool ModuleLoader::parseBootstrap(const RuntimeValue &value,
                                  const abi::LoadVerifiedModule &message,
                                  ModuleError &failure) const noexcept {
  const auto *object = std::get_if<RuntimeValue::Object>(&value.storage());
  if (!object || !exactFields(*object, {"schemaVersion", "kind", "moduleId"}, {"templateId"})) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "invalid bootstrap metadata");
    return false;
  }
  const auto *version = objectField(*object, "schemaVersion");
  const auto *kind = stringField(*object, "kind");
  const auto *id = stringField(*object, "moduleId");
  if (!version || !std::holds_alternative<double>(version->storage()) ||
      std::get<double>(version->storage()) != 1 || !kind || !id ||
      !message.expectedBootstrap || *kind != message.expectedBootstrap->kind ||
      *id != message.expectedBootstrap->moduleId) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "bootstrap expectation mismatch");
    return false;
  }
  if (message.expectedBootstrap->templateId) {
    const auto *templateId = stringField(*object, "templateId");
    if (!templateId || *templateId != *message.expectedBootstrap->templateId) {
      failure = error(ModuleErrorCode::ModuleAbiUnsupported, "template expectation mismatch");
      return false;
    }
  } else if (object->contains("templateId")) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                    "unexpected template expectation");
    return false;
  }
  return true;
}

NativeFunctionResult ModuleLoader::invokeBootstrap(const NativeCallView &call) noexcept {
  if (!currentTransaction_) return NativeFunctionResult::failure({RuntimeErrorCode::JsException, "bootstrap outside load"});
  if (call.args.size() != 2 || currentTransaction_->bootstrapCount != 0) {
    currentTransaction_->failure = error(ModuleErrorCode::ModuleAbiUnsupported, "bootstrap count mismatch");
    return NativeFunctionResult::failure({RuntimeErrorCode::ModuleAbiUnsupported, "bootstrap count mismatch"});
  }
  auto id = engine_->toRuntimeValue(
      *context_, *detail::RefAccess::viewedValue(call.args[0]), {64, 2048});
  auto metadata = engine_->toRuntimeValue(
      *context_, *detail::RefAccess::viewedValue(call.args[1]), {64, 2048});
  const auto *text = id.ok() ? std::get_if<std::string>(&id.value().storage()) : nullptr;
  ModuleError failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                              "bootstrap failed");
  const auto *object = metadata.ok()
                           ? std::get_if<RuntimeValue::Object>(
                                 &metadata.value().storage())
                           : nullptr;
  const auto *version = object ? objectField(*object, "schemaVersion") : nullptr;
  const auto *kind = object ? stringField(*object, "kind") : nullptr;
  const auto *module = object ? stringField(*object, "moduleId") : nullptr;
  const auto *templateId = object ? stringField(*object, "templateId") : nullptr;
  if (!text || *text != currentTransaction_->moduleId || !object ||
      !exactFields(*object, {"schemaVersion", "kind", "moduleId"},
                   {"templateId"}) ||
      !version || !std::holds_alternative<double>(version->storage()) ||
      std::get<double>(version->storage()) != 1 || !kind || !module ||
      (object->contains("templateId") && !templateId)) {
    currentTransaction_->failure = failure;
    return NativeFunctionResult::failure(
        {RuntimeErrorCode::ModuleAbiUnsupported, failure.message});
  }
  currentTransaction_->bootstrapKind = *kind;
  currentTransaction_->bootstrapModuleId = *module;
  if (templateId) currentTransaction_->bootstrapTemplateId = *templateId;
  currentTransaction_->bootstrapCount = 1;
  return nullValue();
}

NativeFunctionResult ModuleLoader::invokeRequire(const NativeCallView &call) noexcept {
  if (!currentEvaluation_ || call.args.size() != 1) {
    return NativeFunctionResult::failure({RuntimeErrorCode::JsException, "require outside module evaluation"});
  }
  auto value = engine_->toRuntimeValue(
      *context_, *detail::RefAccess::viewedValue(call.args[0]), {64, 2048});
  const auto *moduleId = value.ok() ? std::get_if<std::string>(&value.value().storage()) : nullptr;
  ModuleError failure = error(ModuleErrorCode::ModuleAbiUnsupported, "invalid require specifier");
  if (!moduleId) {
    currentEvaluation_->failure = failure;
    return NativeFunctionResult::failure({RuntimeErrorCode::ModuleAbiUnsupported, failure.message});
  }
  auto result = requireModule(*moduleId, failure);
  if (!result.valid()) {
    currentEvaluation_->failure = failure;
    return NativeFunctionResult::failure({RuntimeErrorCode::ModuleAbiUnsupported, failure.message});
  }
  return NativeFunctionResult::success(std::move(result));
}

std::string ModuleLoader::cacheKey(const abi::LoadVerifiedModule &message) const {
  std::string key = appRuntimeId_ + ":" + message.packageId + ":" +
                    message.moduleKind + ":" + message.moduleId + ":" +
                    message.bundle.path + ":" +
                    std::to_string(message.bundle.byteLength) + ":" +
                    message.bundle.sha256;
  for (const auto &dependency : message.dependencies)
    key += ":d" + dependency;
  if (message.expectedBootstrap) key += ":" + message.expectedBootstrap->kind + ":" + message.expectedBootstrap->moduleId + ":" + message.expectedBootstrap->templateId.value_or("");
  if (message.expectedBindingIds) {
    auto ids = *message.expectedBindingIds;
    std::sort(ids.begin(), ids.end());
    for (auto id : ids) key += ":b" + std::to_string(id);
  }
  if (message.expectedHandlerIds) {
    auto ids = *message.expectedHandlerIds;
    std::sort(ids.begin(), ids.end());
    for (auto id : ids) key += ":h" + std::to_string(id);
  }
  return key;
}

bool ModuleLoader::cacheIdentityConflict(const abi::LoadVerifiedModule &message) const noexcept {
  for (const auto &[_, entry] : entries_) {
    if (entry->packageId == message.packageId && entry->moduleKind == message.moduleKind && entry->moduleId == message.moduleId && entry->cacheKey != cacheKey(message)) return true;
  }
  return false;
}

ModuleError ModuleLoader::mapEngineError(const EngineException &exception) const {
  if (exception.kind == EngineExceptionKind::OutOfMemory) return error(ModuleErrorCode::OutOfMemory, exception.message, true);
  if (exception.kind == EngineExceptionKind::Syntax) return error(ModuleErrorCode::JsException, exception.message);
  return error(ModuleErrorCode::JsException, exception.message, true);
}

bool ModuleLoader::deterministicFailure(const ModuleError &failure) const noexcept {
  return failure.code == ModuleErrorCode::ModuleAbiUnsupported ||
         (failure.code == ModuleErrorCode::JsException && !failure.retryable);
}

bool ModuleLoader::evaluateBundle(const abi::LoadVerifiedModule &message,
                                  Transaction &transaction,
                                  ModuleError &failure) noexcept {
  const auto bytes = std::span<const std::uint8_t>(message.bundle.bytes->data(), message.bundle.bytes->size());
  if (sha256(bytes) != message.bundle.sha256) {
    failure = error(ModuleErrorCode::PackageIntegrityFailed, "Bundle SHA-256 mismatch");
    return false;
  }
  if (!validUtf8(bytes)) {
    failure = error(ModuleErrorCode::JsException, "Bundle is not UTF-8");
    return false;
  }
  transaction.moduleId = message.moduleId;
  transaction.moduleKind = message.moduleKind;
  transaction.packageId = message.packageId;
  transaction.dependencies = message.dependencies;
  transaction.sourceBytes = bytes.size();
  std::string source(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  currentTransaction_ = &transaction;
  currentEvaluation_ = &transaction;
  auto evaluated = engine_->evaluate(*context_, SourceUnit{
      "module:" + message.moduleId, message.bundle.path, std::move(source), SourceMode::Script});
  currentEvaluation_ = nullptr;
  currentTransaction_ = nullptr;
  if (!evaluated.ok()) {
    failure = transaction.failure.value_or(mapEngineError(evaluated.error()));
    return false;
  }
  auto evaluatedValue = std::move(evaluated).value();
  evaluatedValue.reset();
  if (transaction.defineCount != 1 ||
      ((message.moduleKind == "app" || message.moduleKind == "page") && transaction.bootstrapCount != 1) ||
      (message.moduleKind == "shared" && transaction.bootstrapCount != 0)) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "define/bootstrap cardinality mismatch");
    return false;
  }
  if (transaction.dependencies != message.dependencies) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                    "define dependency list mismatch");
    return false;
  }
  if (transaction.bootstrapCount != 0) {
    RuntimeValue::Object metadata{{"schemaVersion", RuntimeValue(1.0)}, {"kind", RuntimeValue(transaction.bootstrapKind)}, {"moduleId", RuntimeValue(transaction.bootstrapModuleId)}};
    if (transaction.bootstrapTemplateId) metadata.emplace("templateId", RuntimeValue(*transaction.bootstrapTemplateId));
    if (!parseBootstrap(RuntimeValue(std::move(metadata)), message, failure)) return false;
  }
  return true;
}

bool ModuleLoader::evaluateFactory(Entry &entry, ModuleError &failure) noexcept {
  if (entry.state == Entry::State::Loaded) return true;
  if (entry.state == Entry::State::Failed && entry.failure) {
    failure = *entry.failure;
    return false;
  }
  if (evaluationStack_.size() >= limits_.maxEvaluationDepth || std::find(evaluationStack_.begin(), evaluationStack_.end(), entry.cacheKey) != evaluationStack_.end()) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported, "module dependency cycle");
    return false;
  }
  evaluationStack_.push_back(entry.cacheKey);
  entry.state = Entry::State::Evaluating;
  EvaluationContext context{entry.moduleId, entry.dependencies, std::nullopt};
  auto *previous = currentEvaluation_;
  currentEvaluation_ = &context;
  for (const auto &dependency : entry.dependencies) {
    Entry *dependencyEntry = nullptr;
    for (auto &[_, candidate] : entries_) {
      if (candidate->moduleId == dependency && candidate->moduleKind != "page") {
        dependencyEntry = candidate.get();
        break;
      }
    }
    if (!dependencyEntry || !evaluateFactory(*dependencyEntry, failure)) {
      currentEvaluation_ = previous;
      evaluationStack_.pop_back();
      entry.state = Entry::State::Defined;
      return false;
    }
  }
  auto global = engine_->globalObject(*context_);
  auto require = global.ok() ? engine_->getProperty(*context_, global.value(), "$app_require$") : EngineResult<JsValueRef>::failure(EngineException{EngineExceptionKind::Runtime, "require binding missing", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  auto exports = engine_->fromRuntimeValue(*context_, RuntimeValue(RuntimeValue::Object{}));
  auto module = engine_->fromRuntimeValue(*context_, RuntimeValue(RuntimeValue::Object{{"exports", RuntimeValue(RuntimeValue::Object{})}}));
  bool success = global.ok() && require.ok() && exports.ok() && module.ok();
  JsValueRef moduleExports;
  if (success) {
    auto set = engine_->setProperty(*context_, module.value(), "exports", exports.value());
    success = set.ok();
  }
  if (success) {
    auto thisValue = engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
    success = thisValue.ok();
    if (!success) {
      failure = mapEngineError(thisValue.error());
    }
    if (success) {
      std::array<JsValueRef, 3> args{std::move(require).value(),
                                    std::move(module).value(),
                                    std::move(exports).value()};
      auto result = engine_->call(*context_, entry.factory,
                                  std::move(thisValue).value(),
                                  std::span<const JsValueRef>(args.data(),
                                                               args.size()));
    success = result.ok();
    if (success) {
      auto object = engine_->getProperty(*context_, args[1], "exports");
      success = object.ok();
      if (success) moduleExports = std::move(object).value();
    }
    }
  }
  currentEvaluation_ = previous;
  evaluationStack_.pop_back();
  if (!success) {
    failure = context.failure.value_or(error(ModuleErrorCode::JsException, "module factory failed", true));
    entry.state = Entry::State::Defined;
    return false;
  }
  entry.exports = std::move(moduleExports);
  entry.state = Entry::State::Loaded;
  return true;
}

JsValueRef ModuleLoader::requireModule(std::string_view moduleId,
                                       ModuleError &failure) noexcept {
  if (!currentEvaluation_) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                    "require is outside module evaluation");
    return {};
  }
  const auto declared =
      std::find(currentEvaluation_->dependencies.begin(),
                currentEvaluation_->dependencies.end(), moduleId) !=
      currentEvaluation_->dependencies.end();
  if (declared) {
    for (auto &[_, entry] : entries_) {
      if (entry->moduleId == moduleId && entry->moduleKind != "page") {
        if (entry->state != Entry::State::Loaded) {
          failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                          "dependency factory is not initialized");
          return {};
        }
        auto retained = engine_->retain(*context_, entry->exports);
        if (!retained.ok()) {
          failure = mapEngineError(retained.error());
          return {};
        }
        return std::move(retained).value();
      }
    }
    failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                    "declared module is not loaded");
    return {};
  }
  if (frameworkModules_) {
    auto resolved = frameworkModules_->resolveOnExecutor(moduleId);
    if (resolved.ok()) return std::move(resolved).value();
    failure = resolved.error();
    return {};
  }
  failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                  "module is neither a dependency nor a Framework builtin");
  return {};
}

bool ModuleLoader::validateDefinition(const JsValueRef &exports,
                                      std::string_view moduleKind,
                                      const abi::LoadVerifiedModule &message,
                                      Entry &entry, ModuleError &failure) noexcept {
  auto kind = engine_->fromRuntimeValue(*context_, RuntimeValue(std::string(moduleKind)));
  auto null = engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
  if (!kind.ok() || !null.ok()) { failure = error(ModuleErrorCode::OutOfMemory, "definition validation allocation failed", true); return false; }
  auto retainedExports = engine_->retain(*context_, exports);
  if (!retainedExports.ok()) {
    failure = mapEngineError(retainedExports.error());
    return false;
  }
  std::array<JsValueRef, 2> args{std::move(retainedExports).value(),
                                 std::move(kind).value()};
  auto result = engine_->call(*context_, definitionValidator_, null.value(), std::span<const JsValueRef>(args.data(), args.size()));
  if (!result.ok()) { failure = mapEngineError(result.error()); return false; }
  auto parsed = engine_->toRuntimeValue(*context_, result.value(), {64, 4096});
  if (!parsed.ok()) { failure = mapEngineError(parsed.error()); return false; }
  const auto *object = std::get_if<RuntimeValue::Object>(&parsed.value().storage());
  const auto *ok = object ? objectField(*object, "ok") : nullptr;
  if (!ok || !std::get_if<bool>(&ok->storage()) || !std::get<bool>(ok->storage())) { failure = error(ModuleErrorCode::ModuleAbiUnsupported, "Definition shape rejected"); return false; }
  const auto createName = moduleKind == "app" ? "createAppVm" : "createPageVm";
  auto create = engine_->getProperty(*context_, exports, createName);
  auto callable = create.ok()
                      ? engine_->isCallable(*context_, create.value())
                      : EngineResult<bool>::failure(EngineException{
                            EngineExceptionKind::Runtime,
                            "VM factory is missing", std::nullopt,
                            std::nullopt, std::nullopt, std::nullopt});
  if (!callable.ok() || !callable.value()) { failure = error(ModuleErrorCode::ModuleAbiUnsupported, "VM factory is not callable"); return false; }
  entry.createVm = std::move(create).value();
  auto readIds = [&](std::string_view name,
                     const std::optional<std::vector<std::uint64_t>> &expected,
                     std::vector<std::uint64_t> *parsedIds) {
    const auto *ids = objectField(*object, name);
    if (!ids || !std::get_if<RuntimeValue::Array>(&ids->storage())) return false;
    std::set<std::uint64_t> actual;
    for (const auto &id : std::get<RuntimeValue::Array>(ids->storage())) {
      const auto *text = std::get_if<std::string>(&id.storage());
      if (!text || text->empty() || text->front() == '0' || !std::all_of(text->begin(), text->end(), [](char c){return c>='0'&&c<='9';})) return false;
      std::uint64_t number = 0;
      if (!positiveDecimal(*text, number) || !actual.insert(number).second)
        return false;
    }
    if (parsedIds) parsedIds->assign(actual.begin(), actual.end());
    if (!expected) return true;
    std::set<std::uint64_t> wanted(expected->begin(), expected->end());
    return actual == wanted;
  };
  if (moduleKind == "page") {
    auto evaluators = engine_->getProperty(*context_, exports, "bindingEvaluators");
    if (!evaluators.ok()) {
      failure = mapEngineError(evaluators.error());
      return false;
    }
    auto retainedEvaluators = engine_->retain(*context_, evaluators.value());
    if (!retainedEvaluators.ok()) {
      failure = mapEngineError(retainedEvaluators.error());
      return false;
    }
    entry.bindingEvaluators = std::move(retainedEvaluators).value();
    auto handlers = engine_->getProperty(*context_, exports, "handlerMethods");
    if (!handlers.ok()) {
      failure = mapEngineError(handlers.error());
      return false;
    }
    auto handlerValue = engine_->toRuntimeValue(*context_, handlers.value(),
                                                {16, 512});
    if (!handlerValue.ok() ||
        !std::get_if<RuntimeValue::Object>(&handlerValue.value().storage())) {
      failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                      "handlerMethods is not an object");
      return false;
    }
    for (const auto& [id, method] :
         std::get<RuntimeValue::Object>(handlerValue.value().storage())) {
      std::uint64_t numeric = 0;
      if (id.empty() || id.front() == '0' ||
          !std::all_of(id.begin(), id.end(),
                       [](char value) { return value >= '0' && value <= '9'; }) ||
          !positiveDecimal(id, numeric) ||
          !std::get_if<std::string>(&method.storage())) {
        failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                        "handlerMethods contains an invalid entry");
        return false;
      }
      entry.handlerMethods.emplace(numeric,
                                   std::get<std::string>(method.storage()));
    }
  }
  if (!readIds("bindingIds", message.expectedBindingIds,
               moduleKind == "page" ? &entry.bindingIds : nullptr) ||
      !readIds("handlerIds", message.expectedHandlerIds, nullptr)) {
    failure = error(ModuleErrorCode::ModuleAbiUnsupported,
                    "Definition ID set mismatch");
    return false;
  }
  return true;
}

bool ModuleLoader::commit(Transaction &transaction,
                          const abi::LoadVerifiedModule &message,
                          ModuleError &failure) noexcept {
  if (entries_.size() >= limits_.maxEntries) { failure = error(ModuleErrorCode::QueueOverflow, "module entry capacity reached", true); return false; }
  auto entry = std::make_unique<Entry>();
  entry->handle = ModuleDefinitionHandle{++nextEntryId_, ++nextGeneration_, message.moduleKind, message.moduleId};
  entry->cacheKey = transaction.cacheKey;
  entry->packageId = message.packageId;
  entry->moduleKind = message.moduleKind;
  entry->moduleId = message.moduleId;
  entry->dependencies = transaction.dependencies;
  entry->sha256 = message.bundle.sha256;
  entry->templateId = transaction.bootstrapTemplateId;
  entry->factory = std::move(transaction.factory);
  if (message.moduleKind != "shared") {
    if (!evaluateFactory(*entry, failure)) return false;
    if (!validateDefinition(entry->exports, message.moduleKind, message, *entry, failure)) return false;
  }
  if (message.moduleKind == "page") {
    std::size_t liveLeases = 0;
    for (const auto &[_, existing] : entries_) liveLeases += existing->leases.size();
    if (liveLeases >= limits_.maxPageLeases) {
      failure = error(ModuleErrorCode::QueueOverflow,
                      "page lease capacity reached", true);
      return false;
    }
    entry->leases.insert(*message.surfaceId);
  }
  entries_.emplace(entry->cacheKey, std::move(entry));
  return true;
}

ModuleEnqueueResult ModuleLoader::complete(ModuleLoadCompletion completion) noexcept {
  const auto requestId = completion.requestId;
  auto result = completion_.post(completion);
  if (result.status == ModuleEnqueueStatus::QueueOverflow && outbox_.size() < limits_.maxOutbox) {
    // The request ID is unique for an accepted completion, so it is a stable retry key.
    outbox_.emplace(requestId, std::move(completion));
    return {ModuleEnqueueStatus::Accepted};
  }
  return result;
}

void ModuleLoader::cacheDeterministicFailure(
    const abi::LoadVerifiedModule &message, std::string_view key,
    ModuleError failure) noexcept {
  if (!deterministicFailure(failure) || entries_.contains(key) ||
      entries_.size() >= limits_.maxEntries) {
    return;
  }
  auto entry = std::make_unique<Entry>();
  entry->handle = ModuleDefinitionHandle{++nextEntryId_, ++nextGeneration_,
                                         message.moduleKind, message.moduleId};
  entry->cacheKey = std::string(key);
  entry->packageId = message.packageId;
  entry->moduleKind = message.moduleKind;
  entry->moduleId = message.moduleId;
  entry->sha256 = message.bundle.sha256;
  entry->state = Entry::State::Failed;
  entry->failure = std::move(failure);
  entries_.emplace(entry->cacheKey, std::move(entry));
}

void ModuleLoader::completeSuccess(const abi::LoadVerifiedModule &message) noexcept {
  static_cast<void>(engineService_.observation().emitBridge(
      "module.load.completed", appRuntimeId_, message.requestId,
      message.surfaceId ? std::optional<std::string_view>(*message.surfaceId)
                        : std::nullopt));
  static_cast<void>(complete(ModuleLoadCompletion{message.requestId, message.moduleKind, message.moduleId, "loaded", message.surfaceId, std::nullopt}));
}

void ModuleLoader::completeFailure(const abi::LoadVerifiedModule &message,
                                   ModuleError failure) noexcept {
  const auto errorCode = moduleErrorCodeName(failure.code);
  static_cast<void>(engineService_.observation().emitBridge(
      "module.load.failed", appRuntimeId_, message.requestId,
      message.surfaceId ? std::optional<std::string_view>(*message.surfaceId)
                        : std::nullopt,
      errorCode));
  static_cast<void>(complete(ModuleLoadCompletion{message.requestId, message.moduleKind, message.moduleId, "failed", message.surfaceId, std::move(failure)}));
}

void ModuleLoader::loadOnExecutor(const abi::LoadVerifiedModule &message) noexcept {
  if (!onExecutor() || !running_) return;
  try {
    ModuleError failure = error(ModuleErrorCode::InvalidArgument, "module load rejected");
    if (!validateInput(message, failure)) { completeFailure(message, failure); return; }
    const auto key = cacheKey(message);
    const auto requestFingerprint = key + ":" + message.requestId;
    if (auto request = terminalRequests_.find(message.requestId);
        request != terminalRequests_.end()) {
      if (request->second == requestFingerprint) return;
      completeFailure(message, error(ModuleErrorCode::InvalidArgument,
                                     "requestId payload collision"));
      return;
    }
    if (terminalRequests_.size() >= limits_.maxRequestRecords) {
      completeFailure(message,
                      error(ModuleErrorCode::QueueOverflow,
                            "request ledger capacity reached", true));
      return;
    }
    terminalRequests_.emplace(message.requestId, requestFingerprint);
    static_cast<void>(engineService_.observation().emitBridge(
        "module.load.started", appRuntimeId_, message.requestId,
        message.surfaceId ? std::optional<std::string_view>(*message.surfaceId)
                          : std::nullopt));
    if (cacheIdentityConflict(message)) { completeFailure(message, error(ModuleErrorCode::ModuleAbiUnsupported, "module identity conflict")); return; }
    if (auto found = entries_.find(key); found != entries_.end()) {
      if (found->second->state == Entry::State::Failed && found->second->failure) completeFailure(message, *found->second->failure);
      else {
        if (message.moduleKind == "page" &&
            !found->second->leases.contains(*message.surfaceId)) {
          std::size_t liveLeases = 0;
          for (const auto &[_, entry] : entries_)
            liveLeases += entry->leases.size();
          if (liveLeases >= limits_.maxPageLeases) {
            completeFailure(message,
                            error(ModuleErrorCode::QueueOverflow,
                                  "page lease capacity reached", true));
            return;
          }
          found->second->leases.insert(*message.surfaceId);
        }
        completeSuccess(message);
      }
      return;
    }
    if (retainedBytes_ + message.bundle.byteLength > limits_.maxSourceBytes) { completeFailure(message, error(ModuleErrorCode::OutOfMemory, "module byte budget reached", true)); return; }
    Transaction transaction;
    transaction.cacheKey = key;
    if (!evaluateBundle(message, transaction, failure)) {
      cacheDeterministicFailure(message, key, failure);
      completeFailure(message, failure);
      return;
    }
    if (!commit(transaction, message, failure)) {
      cacheDeterministicFailure(message, key, failure);
      completeFailure(message, failure);
      return;
    }
    completeSuccess(message);
  } catch (const std::bad_alloc &) {
    completeFailure(message, error(ModuleErrorCode::OutOfMemory, "module load allocation failed", true));
  } catch (...) {
    completeFailure(message, error(ModuleErrorCode::JsException, "module load failed", true));
  }
}

void ModuleLoader::onLoadVerifiedModule(const abi::LoadVerifiedModule &message) noexcept {
  loadOnExecutor(message);
}

void ModuleLoader::retryCompletionsOnExecutor() noexcept {
  if (!onExecutor() || !running_) return;
  for (auto it = outbox_.begin(); it != outbox_.end();) {
    auto result = completion_.post(it->second);
    if (result.status == ModuleEnqueueStatus::Accepted) it = outbox_.erase(it);
    else ++it;
  }
}

std::optional<ModuleDefinitionHandle>
ModuleLoader::definitionHandleOnExecutor(std::string_view moduleKind,
                                         std::string_view moduleId) const noexcept {
  if (!onExecutor()) return std::nullopt;
  for (const auto &[_, entry] : entries_) {
    if (entry->moduleKind == moduleKind && entry->moduleId == moduleId &&
        entry->state != Entry::State::Failed)
      return entry->handle;
  }
  return std::nullopt;
}

std::optional<ModuleDefinitionHandle>
ModuleLoader::appDefinitionOnExecutor() const noexcept {
  if (!onExecutor()) return std::nullopt;
  for (const auto &[_, entry] : entries_) {
    if (entry->moduleKind == "app" && entry->state == Entry::State::Loaded)
      return entry->handle;
  }
  return std::nullopt;
}

std::optional<ModuleDefinitionHandle>
ModuleLoader::pageDefinitionForSurfaceOnExecutor(
    std::string_view surfaceId, std::string_view templateId) const noexcept {
  if (!onExecutor()) return std::nullopt;
  const auto surface = surfaces_.find(surfaceId);
  if (surface == surfaces_.end() || !surface->second.open) return std::nullopt;
  for (const auto &[_, entry] : entries_) {
    if (entry->moduleKind == "page" && entry->state == Entry::State::Loaded &&
        entry->leases.contains(surfaceId) && entry->templateId &&
        *entry->templateId == templateId) {
      return entry->handle;
    }
  }
  return std::nullopt;
}

Result<std::vector<abi::HandlerBinding>, ModuleError>
ModuleLoader::handlerBindingsOnExecutor(
    const ModuleDefinitionHandle &definition,
    std::string_view ownerInstanceId) const noexcept {
  if (!onExecutor() || !definition.valid() || ownerInstanceId.empty()) {
    return Result<std::vector<abi::HandlerBinding>, ModuleError>::failure(
        error(ModuleErrorCode::InvalidArgument,
              "handler binding request is invalid"));
  }
  const Entry* entry = nullptr;
  for (const auto& [_, candidate] : entries_) {
    if (candidate->handle.id == definition.id &&
        candidate->handle.generation == definition.generation) {
      entry = candidate.get();
      break;
    }
  }
  if (entry == nullptr || entry->moduleKind != "page" ||
      entry->state != Entry::State::Loaded) {
    return Result<std::vector<abi::HandlerBinding>, ModuleError>::failure(
        error(ModuleErrorCode::InvalidArgument,
              "page definition is unavailable"));
  }
  try {
    std::vector<abi::HandlerBinding> bindings;
    bindings.reserve(entry->handlerMethods.size());
    for (const auto& [templateId, method] : entry->handlerMethods) {
      (void)method;
      bindings.push_back(abi::HandlerBinding{
          std::string(ownerInstanceId), templateId,
          "hdl:" + std::to_string(templateId)});
    }
    return Result<std::vector<abi::HandlerBinding>, ModuleError>::success(
        std::move(bindings));
  } catch (...) {
    return Result<std::vector<abi::HandlerBinding>, ModuleError>::failure(
        error(ModuleErrorCode::OutOfMemory,
              "handler binding allocation failed"));
  }
}

std::optional<std::string> ModuleLoader::handlerMethodNameOnExecutor(
    const ModuleDefinitionHandle &definition,
    std::uint64_t templateHandlerId) const noexcept {
  if (!onExecutor() || !definition.valid()) return std::nullopt;
  for (const auto& [_, candidate] : entries_) {
    if (candidate->handle.id == definition.id &&
        candidate->handle.generation == definition.generation &&
        candidate->state == Entry::State::Loaded &&
        candidate->moduleKind == "page") {
      const auto found = candidate->handlerMethods.find(templateHandlerId);
      if (found != candidate->handlerMethods.end()) return found->second;
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<PageModuleLease>
ModuleLoader::pageLeaseOnExecutor(std::string_view surfaceId,
                                  std::string_view moduleId) const noexcept {
  if (!onExecutor()) return std::nullopt;
  const auto surface = surfaces_.find(surfaceId);
  if (surface == surfaces_.end() || !surface->second.open) return std::nullopt;
  for (const auto &[_, entry] : entries_) {
    if (entry->moduleKind == "page" && entry->moduleId == moduleId &&
        entry->state == Entry::State::Loaded &&
        entry->leases.contains(surfaceId)) {
      return PageModuleLease{std::string(surfaceId), surface->second.generation,
                             std::string(moduleId), entry->handle.generation};
    }
  }
  return std::nullopt;
}

ModuleLoader::Entry *
ModuleLoader::findEntry(const ModuleDefinitionHandle &definition) noexcept {
  if (!onExecutor() || !definition.valid()) return nullptr;
  for (auto &[_, entry] : entries_) {
    if (entry->handle.id == definition.id &&
        entry->handle.generation == definition.generation &&
        entry->handle.moduleKind == definition.moduleKind &&
        entry->handle.moduleId == definition.moduleId &&
        entry->state == Entry::State::Loaded) {
      return entry.get();
    }
  }
  return nullptr;
}

Result<JsValueRef, ModuleError>
ModuleLoader::createVmOnExecutor(const ModuleDefinitionHandle &definition,
                                 const RuntimeValue &context) noexcept {
  auto *entry = findEntry(definition);
  if (!entry || !engine_ || !context_) {
    return Result<JsValueRef, ModuleError>::failure(
        error(ModuleErrorCode::InvalidArgument, "VM Definition is unavailable"));
  }
  auto argument = engine_->fromRuntimeValue(*context_, context);
  auto null = engine_->fromRuntimeValue(*context_, RuntimeValue(nullptr));
  if (!argument.ok()) return Result<JsValueRef, ModuleError>::failure(mapEngineError(argument.error()));
  if (!null.ok()) return Result<JsValueRef, ModuleError>::failure(mapEngineError(null.error()));
  std::array<JsValueRef, 1> args{std::move(argument).value()};
  auto result = engine_->call(*context_, entry->createVm, std::move(null).value(),
                              std::span<const JsValueRef>(args.data(), args.size()));
  if (!result.ok()) return Result<JsValueRef, ModuleError>::failure(mapEngineError(result.error()));
  return Result<JsValueRef, ModuleError>::success(std::move(result).value());
}

Result<std::vector<BindingEvaluatorHandle>, ModuleError>
ModuleLoader::bindingEvaluatorsOnExecutor(
    const ModuleDefinitionHandle &definition) noexcept {
  auto *entry = findEntry(definition);
  if (!entry || entry->moduleKind != "page" || !engine_ || !context_) {
    return Result<std::vector<BindingEvaluatorHandle>, ModuleError>::failure(
        error(ModuleErrorCode::InvalidArgument, "Page Definition is unavailable"));
  }
  std::vector<BindingEvaluatorHandle> evaluators;
  evaluators.reserve(entry->bindingIds.size());
  for (const auto id : entry->bindingIds) {
    const auto key = std::to_string(id);
    auto evaluator = engine_->getProperty(*context_, entry->bindingEvaluators, key);
    if (!evaluator.ok()) {
      return Result<std::vector<BindingEvaluatorHandle>, ModuleError>::failure(
          mapEngineError(evaluator.error()));
    }
    auto callable = engine_->isCallable(*context_, evaluator.value());
    if (!callable.ok() || !callable.value()) {
      return Result<std::vector<BindingEvaluatorHandle>, ModuleError>::failure(
          error(ModuleErrorCode::ModuleAbiUnsupported, "Binding evaluator is not callable"));
    }
    auto retained = engine_->retain(*context_, evaluator.value());
    if (!retained.ok()) {
      return Result<std::vector<BindingEvaluatorHandle>, ModuleError>::failure(
          mapEngineError(retained.error()));
    }
    bool initial = true;
    auto marker = engine_->getProperty(*context_, evaluator.value(), "__qak_initial__");
    if (marker.ok()) {
      auto markerValue = engine_->toRuntimeValue(*context_, marker.value(), {4, 8});
      if (markerValue.ok()) {
        if (const auto *flag = std::get_if<bool>(&markerValue.value().storage()))
          initial = *flag;
      }
    }
    evaluators.push_back({id, std::move(retained).value(), initial});
  }
  return Result<std::vector<BindingEvaluatorHandle>, ModuleError>::success(
      std::move(evaluators));
}

void ModuleLoader::releaseAllOnExecutor() noexcept {
  entries_.clear();
  surfaces_.clear();
  outbox_.clear();
  terminalRequests_.clear();
  evaluationStack_.clear();
  currentEvaluation_ = nullptr;
  currentTransaction_ = nullptr;
  retainedBytes_ = 0;
  if (engine_ && context_) {
    for (auto it = bindingTokens_.rbegin(); it != bindingTokens_.rend(); ++it) static_cast<void>(engine_->unbindNativeFunction(*context_, *it));
  }
  bindingTokens_.clear();
  definitionValidator_.reset();
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

void ModuleLoader::stopOnExecutor() noexcept {
  if (!onExecutor()) return;
  releaseAllOnExecutor();
}

ModuleResourceSnapshot ModuleLoader::resources() const noexcept {
  std::size_t leases = 0;
  for (const auto &[_, entry] : entries_) leases += entry->leases.size();
  return {entries_.size(), leases, 0, retainedBytes_, outbox_.size()};
}

} // namespace quickapp::js::module
