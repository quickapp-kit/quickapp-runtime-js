#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "quickapp/js/abi/runtime_abi_types.h"
#include "quickapp/js/engine/js_engine_service.h"

namespace quickapp::js::module {

enum class ModuleErrorCode {
  InvalidArgument,
  PackageIntegrityFailed,
  ModuleAbiUnsupported,
  JsException,
  OutOfMemory,
  QueueOverflow,
  SurfaceNotFound,
  PortClosed,
};

struct ModuleError {
  ModuleErrorCode code{ModuleErrorCode::InvalidArgument};
  std::string message;
  bool retryable{false};
};

struct ModuleLoadCompletion {
  std::string requestId;
  std::string moduleKind;
  std::string moduleId;
  std::string status;
  std::optional<std::string> surfaceId;
  std::optional<ModuleError> error;
};

enum class ModuleEnqueueStatus { Accepted, QueueOverflow, Closed };

struct ModuleEnqueueResult {
  ModuleEnqueueStatus status{ModuleEnqueueStatus::Closed};
  [[nodiscard]] bool accepted() const noexcept {
    return status == ModuleEnqueueStatus::Accepted;
  }
};

class ModuleCompletionPort {
public:
  virtual ~ModuleCompletionPort() = default;
  [[nodiscard]] virtual ModuleEnqueueResult
  post(const ModuleLoadCompletion &completion) noexcept = 0;
};

// Resolver implementations return an already-created immutable Framework facade.
// Lookup may retain that value but must not evaluate or call JavaScript.
class FrameworkModuleResolverPort {
public:
  virtual ~FrameworkModuleResolverPort() = default;
  [[nodiscard]] virtual Result<JsValueRef, ModuleError>
  resolveOnExecutor(std::string_view moduleId) noexcept = 0;
};

struct ModuleLoaderLimits {
  std::size_t maxEntries{128};
  std::size_t maxPageLeases{128};
  std::size_t maxOutbox{64};
  std::size_t maxDependencies{64};
  std::size_t maxExpectedIds{4096};
  std::size_t maxRequestRecords{1024};
  std::size_t maxSourceBytes{2 * 1024 * 1024};
  std::size_t maxEvaluationDepth{32};
};

struct ModuleResourceSnapshot {
  std::size_t liveEntries{0};
  std::size_t livePageLeases{0};
  std::size_t activeLoads{0};
  std::size_t retainedBytes{0};
  std::size_t pendingCompletions{0};
};

struct PageModuleLease {
  std::string surfaceId;
  std::uint64_t surfaceGeneration{0};
  std::string moduleId;
  std::uint64_t definitionGeneration{0};
};

struct ModuleDefinitionHandle {
  std::uint64_t id{0};
  std::uint64_t generation{0};
  std::string moduleKind;
  std::string moduleId;

  [[nodiscard]] bool valid() const noexcept { return id != 0; }
};

struct BindingEvaluatorHandle {
  std::uint64_t templateBindingId{0};
  JsValueRef evaluator;
  bool initial{true};
};

class ModuleLoader final {
public:
  ModuleLoader(JsEngineService &engineService, ModuleCompletionPort &completion,
               std::string appRuntimeId, std::string packageId,
               ModuleLoaderLimits limits = {},
               FrameworkModuleResolverPort *frameworkModules = nullptr);
  ~ModuleLoader();

  ModuleLoader(const ModuleLoader &) = delete;
  ModuleLoader &operator=(const ModuleLoader &) = delete;

  [[nodiscard]] bool startOnExecutor(JsEnginePort &engine,
                                     const JsContextRef &context) noexcept;
  [[nodiscard]] abi::CallbackSlots callbackSlots() noexcept;
  void onLoadVerifiedModule(const abi::LoadVerifiedModule &message) noexcept;
  [[nodiscard]] bool openSurfaceOnExecutor(std::string surfaceId) noexcept;
  [[nodiscard]] bool closeSurfaceOnExecutor(std::string_view surfaceId) noexcept;
  void retryCompletionsOnExecutor() noexcept;
  void stopOnExecutor() noexcept;

  [[nodiscard]] ModuleResourceSnapshot resources() const noexcept;
  [[nodiscard]] std::optional<ModuleDefinitionHandle>
  definitionHandleOnExecutor(std::string_view moduleKind,
                             std::string_view moduleId) const noexcept;
  [[nodiscard]] std::optional<ModuleDefinitionHandle>
  appDefinitionOnExecutor() const noexcept;
  [[nodiscard]] std::optional<ModuleDefinitionHandle>
  pageDefinitionForSurfaceOnExecutor(std::string_view surfaceId,
                                    std::string_view templateId) const noexcept;
  [[nodiscard]] std::optional<PageModuleLease>
  pageLeaseOnExecutor(std::string_view surfaceId,
                      std::string_view moduleId) const noexcept;
  [[nodiscard]] Result<JsValueRef, ModuleError>
  createVmOnExecutor(const ModuleDefinitionHandle &definition,
                     const RuntimeValue &context) noexcept;
  [[nodiscard]] Result<std::vector<BindingEvaluatorHandle>, ModuleError>
  bindingEvaluatorsOnExecutor(
      const ModuleDefinitionHandle &definition) noexcept;
  [[nodiscard]] Result<std::vector<abi::HandlerBinding>, ModuleError>
  handlerBindingsOnExecutor(const ModuleDefinitionHandle &definition,
                            std::string_view ownerInstanceId) const noexcept;
  [[nodiscard]] std::optional<std::string> handlerMethodNameOnExecutor(
      const ModuleDefinitionHandle &definition,
      std::uint64_t templateHandlerId) const noexcept;

private:
  struct EvaluationContext;
  struct Transaction;
  struct Entry;
  struct SurfaceScope;

  [[nodiscard]] NativeFunctionResult
  invokeDefine(const NativeCallView &call) noexcept;
  [[nodiscard]] NativeFunctionResult
  invokeBootstrap(const NativeCallView &call) noexcept;
  [[nodiscard]] NativeFunctionResult
  invokeRequire(const NativeCallView &call) noexcept;
  [[nodiscard]] NativeFunctionResult nullValue() noexcept;

  void loadOnExecutor(const abi::LoadVerifiedModule &message) noexcept;
  [[nodiscard]] bool evaluateBundle(const abi::LoadVerifiedModule &message,
                                    Transaction &transaction,
                                    ModuleError &error) noexcept;
  [[nodiscard]] bool evaluateFactory(Entry &entry, ModuleError &error) noexcept;
  [[nodiscard]] JsValueRef requireModule(std::string_view moduleId,
                                         ModuleError &error) noexcept;
  [[nodiscard]] bool validateDefinition(const JsValueRef &exports,
                                        std::string_view moduleKind,
                                        const abi::LoadVerifiedModule &message,
                                        Entry &entry,
                                        ModuleError &error) noexcept;
  [[nodiscard]] bool parseBootstrap(const RuntimeValue &value,
                                    const abi::LoadVerifiedModule &message,
                                    ModuleError &error) const noexcept;
  [[nodiscard]] bool parseDefine(const NativeCallView &call,
                                 Transaction &transaction,
                                 ModuleError &error) noexcept;
  [[nodiscard]] bool parseDependencies(const RuntimeValue &value,
                                       std::vector<std::string> &dependencies,
                                       ModuleError &error) const noexcept;
  [[nodiscard]] bool validateInput(const abi::LoadVerifiedModule &message,
                                   ModuleError &error) const noexcept;
  [[nodiscard]] bool commit(Transaction &transaction,
                            const abi::LoadVerifiedModule &message,
                            ModuleError &error) noexcept;
  [[nodiscard]] ModuleError mapEngineError(const EngineException &error) const;
  [[nodiscard]] ModuleEnqueueResult complete(ModuleLoadCompletion completion) noexcept;
  void completeSuccess(const abi::LoadVerifiedModule &message) noexcept;
  void completeFailure(const abi::LoadVerifiedModule &message,
                       ModuleError error) noexcept;
  void releaseAllOnExecutor() noexcept;
  [[nodiscard]] bool cacheIdentityConflict(
      const abi::LoadVerifiedModule &message) const noexcept;
  [[nodiscard]] std::string cacheKey(const abi::LoadVerifiedModule &message) const;
  [[nodiscard]] bool deterministicFailure(const ModuleError &error) const noexcept;
  void cacheDeterministicFailure(const abi::LoadVerifiedModule &message,
                                 std::string_view key,
                                 ModuleError failure) noexcept;
  [[nodiscard]] bool onExecutor() const noexcept;
  [[nodiscard]] Entry *findEntry(const ModuleDefinitionHandle &definition) noexcept;

  JsEngineService &engineService_;
  ModuleCompletionPort &completion_;
  std::string appRuntimeId_;
  std::string packageId_;
  ModuleLoaderLimits limits_;
  FrameworkModuleResolverPort *frameworkModules_{nullptr};
  JsEnginePort *engine_{nullptr};
  const JsContextRef *context_{nullptr};
  std::vector<NativeBindingToken> bindingTokens_;
  JsValueRef definitionValidator_;
  std::uint64_t nextEntryId_{0};
  std::uint64_t nextGeneration_{0};
  std::size_t retainedBytes_{0};
  std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
  std::map<std::string, SurfaceScope, std::less<>> surfaces_;
  std::map<std::string, ModuleLoadCompletion, std::less<>> outbox_;
  std::map<std::string, std::string, std::less<>> terminalRequests_;
  std::vector<std::string> evaluationStack_;
  EvaluationContext *currentEvaluation_{nullptr};
  Transaction *currentTransaction_{nullptr};
  bool running_{false};
};

[[nodiscard]] std::string_view moduleErrorCodeName(ModuleErrorCode code) noexcept;

} // namespace quickapp::js::module
