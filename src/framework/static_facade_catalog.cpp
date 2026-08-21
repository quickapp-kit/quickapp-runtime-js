#include "quickapp/js/framework/static_facade_catalog.h"

#include <exception>
#include <utility>

namespace quickapp::js::framework {
namespace {

module::ModuleError failure(module::ModuleErrorCode code, std::string message) {
  return {code, std::move(message), false};
}

constexpr std::string_view kRouterModule = "@app-module/system.router";
constexpr std::string_view kPromptModule = "@app-module/system.prompt";
constexpr std::string_view kFetchModule = "@app-module/system.fetch";
constexpr std::string_view kRouterFacadeSource = R"JS(
(() => {
  let requestSequence = 100000;
  return Object.freeze({
    default: Object.freeze({
      push: function(options) {
        if (!options || typeof options !== "object" ||
            typeof options.uri !== "string" || options.uri.length === 0 ||
            typeof globalThis.$quickapp_current_surface_id$ !== "string") {
          throw new TypeError("system.router.push requires uri and Surface context");
        }
        return $quickapp_runtime_v1_pushRoute$({
          schemaVersion: 1,
          kind: "navigationPush",
          requestId: "req:j-" + (++requestSequence),
          sourceSurfaceId: globalThis.$quickapp_current_surface_id$,
          uri: options.uri,
          params: options.params || {}
        });
      }
    })
  });
})()
)JS";
constexpr std::string_view kPromptFacadeSource = R"JS(
Object.freeze({ default: Object.freeze({ showToast: function() {} }) })
)JS";
constexpr std::string_view kFetchFacadeSource = R"JS(
Object.freeze({ default: Object.freeze({ fetch: function() { return Promise.reject(new Error("system.fetch is not active in Alpha S1")); } }) })
)JS";

} // namespace

StaticFacadeCatalog::~StaticFacadeCatalog() {
  if (running_)
    std::terminate();
}

bool StaticFacadeCatalog::startOnExecutor(
    JsEnginePort &engine, const JsContextRef &context) noexcept {
  if (running_ || !context.valid())
    return false;
  SourceUnit source{"alpha-system-router", "quickapp://framework/system.router",
                    std::string(kRouterFacadeSource), SourceMode::Script};
  auto facade = engine.evaluate(context, source);
  if (!facade.ok())
    return false;
  auto prompt = engine.evaluate(context, SourceUnit{"alpha-system-prompt", "quickapp://framework/system.prompt", std::string(kPromptFacadeSource), SourceMode::Script});
  if (!prompt.ok()) return false;
  auto fetch = engine.evaluate(context, SourceUnit{"alpha-system-fetch", "quickapp://framework/system.fetch", std::string(kFetchFacadeSource), SourceMode::Script});
  if (!fetch.ok()) return false;
  auto globals = engine.evaluate(context, SourceUnit{"alpha-global-alias", "quickapp://framework/global", "globalThis.global = globalThis;", SourceMode::Script});
  if (!globals.ok()) return false;
  engine_ = &engine;
  context_ = &context;
  routerFacade_ = std::move(facade).value();
  promptFacade_ = std::move(prompt).value();
  fetchFacade_ = std::move(fetch).value();
  running_ = true;
  return true;
}

Result<JsValueRef, module::ModuleError>
StaticFacadeCatalog::resolveOnExecutor(std::string_view moduleId) noexcept {
  if (!running_ || !engine_ || !context_ ||
      (moduleId != kRouterModule && moduleId != kPromptModule && moduleId != kFetchModule)) {
    return Result<JsValueRef, module::ModuleError>::failure(
        failure(module::ModuleErrorCode::ModuleAbiUnsupported,
                "Framework module is not in the Alpha static facade catalog"));
  }
  const auto &facade = moduleId == kRouterModule ? routerFacade_ :
                       moduleId == kPromptModule ? promptFacade_ : fetchFacade_;
  auto retained = engine_->retain(*context_, facade);
  if (!retained.ok()) {
    return Result<JsValueRef, module::ModuleError>::failure(
        failure(retained.error().kind == EngineExceptionKind::OutOfMemory
                    ? module::ModuleErrorCode::OutOfMemory
                    : module::ModuleErrorCode::JsException,
                retained.error().message));
  }
  return Result<JsValueRef, module::ModuleError>::success(
      std::move(retained).value());
}

void StaticFacadeCatalog::stopOnExecutor() noexcept {
  routerFacade_.reset();
  promptFacade_.reset();
  fetchFacade_.reset();
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

} // namespace quickapp::js::framework
