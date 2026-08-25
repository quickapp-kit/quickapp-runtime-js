#include "quickapp/js/framework/static_facade_catalog.h"

#include <exception>
#include <array>
#include <utility>

#include "quickapp/js/abi/runtime_abi_types.h"

namespace quickapp::js::framework {
namespace {

module::ModuleError failure(module::ModuleErrorCode code, std::string message) {
  return {code, std::move(message), false};
}

constexpr std::string_view kRouterModule = "@app-module/system.router";
constexpr std::string_view kPromptModule = "@app-module/system.prompt";
constexpr std::string_view kDeviceModule = "@app-module/system.device";
constexpr std::string_view kTimerModule = "@app-module/system.timer";
constexpr std::string_view kFetchModule = "@app-module/system.fetch";
constexpr std::string_view kFileModule = "@app-module/system.file";
constexpr std::string_view kOpenUrlModule = "@app-module/system.openUrl";
constexpr std::string_view kWebviewModule = "@app-module/system.webview";
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
      },
      back: function() {
        if (typeof globalThis.$quickapp_current_surface_id$ !== "string") {
          throw new TypeError("system.router.back requires Surface context");
        }
        return $quickapp_runtime_v1_closeRoute$({
          schemaVersion: 1,
          kind: "navigationClose",
          requestId: "req:j-" + (++requestSequence),
          sourceSurfaceId: globalThis.$quickapp_current_surface_id$
        });
      }
    })
  });
})()
)JS";
constexpr std::string_view kPromptFacadeSource = R"JS(
(() => {
  let requestSequence = 200000;
  const pending = new Map();
  const request = (method, message) => {
    const requestId = "req:j-" + (++requestSequence);
    return new Promise((resolve, reject) => {
      pending.set(requestId, { resolve, reject });
      const enqueue = $quickapp_runtime_v1_featureRequest$({
        schemaVersion: 1, kind: "featureRequest", requestId,
        surfaceId: globalThis.$quickapp_current_surface_id$,
        module: "prompt", method, text: message
      });
      if (!enqueue || enqueue.ok !== true) {
        pending.delete(requestId);
        reject(enqueue && enqueue.error ? enqueue.error : { code: "QUEUE_REJECTED" });
      }
    });
  };
  return Object.freeze({ default: Object.freeze({
    showToast: function(options) {
      if (!options || typeof options !== "object" ||
          typeof options.message !== "string" || options.message.length === 0 ||
          (options.duration !== undefined &&
           (!Number.isFinite(options.duration) || options.duration < 0)) ||
          typeof globalThis.$quickapp_current_surface_id$ !== "string") {
        throw new TypeError("system.prompt.showToast requires message and Surface context");
      }
      return $quickapp_runtime_v1_showToast$({
        schemaVersion: 1, kind: "showToast",
        requestId: "req:j-" + (++requestSequence),
        surfaceId: globalThis.$quickapp_current_surface_id$,
        message: options.message,
        durationMs: options.duration === undefined ? 0 : options.duration
      });
    },
    alert: function(options) {
      if (!options || typeof options.message !== "string" || !options.message.length || typeof globalThis.$quickapp_current_surface_id$ !== "string") throw new TypeError("system.prompt.alert requires message and Surface context");
      return request("alert", options.message);
    },
    confirm: function(options) {
      if (!options || typeof options.message !== "string" || !options.message.length || typeof globalThis.$quickapp_current_surface_id$ !== "string") throw new TypeError("system.prompt.confirm requires message and Surface context");
      return request("confirm", options.message);
    }
  }), __dispatchResult: function(result) {
    const item = pending.get(result.requestId);
    if (!item) return false;
    pending.delete(result.requestId);
    if (result.status === "completed") item.resolve(result.confirmed === undefined ? true : result.confirmed);
    else item.reject(result.error || { code: result.status });
    return true;
  } });
})()
)JS";
constexpr std::string_view kDeviceFacadeSource = R"JS(
(() => {
  let requestSequence = 300000;
  return Object.freeze({ default: Object.freeze({
    getInfo: function() {
      if (typeof globalThis.$quickapp_current_surface_id$ !== "string") {
        throw new TypeError("system.device.getInfo requires Surface context");
      }
      return $quickapp_runtime_v1_getDeviceInfo$({
        schemaVersion: 1, kind: "deviceGetInfo",
        requestId: "req:j-" + (++requestSequence),
        surfaceId: globalThis.$quickapp_current_surface_id$
      });
    }
  }) });
})()
)JS";
constexpr std::string_view kFetchFacadeSource = R"JS(
(() => {
  let requestSequence = 500000;
  const pending = new Map();
  const request = payload => new Promise((resolve, reject) => {
    pending.set(payload.requestId, { resolve, reject });
    const enqueue = $quickapp_runtime_v1_featureRequest$(payload);
    if (!enqueue || enqueue.ok !== true) {
      pending.delete(payload.requestId);
      reject(enqueue && enqueue.error ? enqueue.error : { code: "QUEUE_REJECTED" });
    }
  });
  const fetch = function(options) {
    if (!options || typeof options.url !== "string" || !options.url.length || typeof globalThis.$quickapp_current_surface_id$ !== "string") throw new TypeError("system.fetch.fetch requires url and Surface context");
    return request({ schemaVersion: 1, kind: "featureRequest", requestId: "req:j-" + (++requestSequence), surfaceId: globalThis.$quickapp_current_surface_id$, module: "fetch", method: "fetch", url: options.url, httpMethod: options.method || "GET", headers: options.headers || {}, body: options.body, timeoutMs: options.timeout || 0, responseType: options.responseType || "text" });
  };
  const cancel = function(requestId) { if (typeof requestId !== "string" || !requestId.startsWith("req:j-")) throw new TypeError("system.fetch.cancel requires requestId"); return $quickapp_runtime_v1_featureRequest$({ schemaVersion: 1, kind: "featureRequest", requestId: "req:j-" + (++requestSequence), surfaceId: globalThis.$quickapp_current_surface_id$, module: "fetch", method: "cancel", targetRequestId: requestId }); };
  return Object.freeze({ default: Object.freeze({ fetch, cancel }), __dispatchResult: function(result) {
    const item = pending.get(result.requestId);
    if (!item) return false;
    pending.delete(result.requestId);
    if (result.status === "completed") item.resolve(result);
    else item.reject(result.error || { code: result.status });
    return true;
  } });
})()
)JS";
constexpr std::string_view kFileFacadeSource = R"JS(
(() => {
  let requestSequence = 600000;
  const pending = new Map();
  const call = (method, options) => {
    if (!options || typeof options.path !== "string" || !options.path.startsWith("private/") || typeof globalThis.$quickapp_current_surface_id$ !== "string") throw new TypeError("system.file requires private path and Surface context");
    const requestId = "req:j-" + (++requestSequence);
    return new Promise((resolve, reject) => {
      pending.set(requestId, { resolve, reject });
      const enqueue = $quickapp_runtime_v1_featureRequest$({ schemaVersion: 1, kind: "featureRequest", requestId, surfaceId: globalThis.$quickapp_current_surface_id$, module: "file", method, path: options.path, data: options.data });
      if (!enqueue || enqueue.ok !== true) {
        pending.delete(requestId);
        reject(enqueue && enqueue.error ? enqueue.error : { code: "QUEUE_REJECTED" });
      }
    });
  };
  return Object.freeze({ default: Object.freeze({ read: options => call("read", options), write: options => call("write", options), exists: options => call("exists", options), delete: options => call("delete", options) }), __dispatchResult: function(result) {
    const item = pending.get(result.requestId);
    if (!item) return false;
    pending.delete(result.requestId);
    if (result.status === "completed") item.resolve(result);
    else item.reject(result.error || { code: result.status });
    return true;
  } });
})()
)JS";
constexpr std::string_view kOpenUrlFacadeSource = R"JS(
(() => {
  let requestSequence = 800000;
  const pending = new Map();
  const open = function(options) {
    if (!options || typeof options.url !== "string" || !/^https?:\/\//.test(options.url) || typeof globalThis.$quickapp_current_surface_id$ !== "string") throw new TypeError("system.openUrl.open requires an http(s) URL and Surface context");
    const requestId = "req:j-" + (++requestSequence);
    return new Promise((resolve, reject) => {
      pending.set(requestId, { resolve, reject });
      const enqueue = $quickapp_runtime_v1_featureRequest$({ schemaVersion: 1, kind: "featureRequest", requestId, surfaceId: globalThis.$quickapp_current_surface_id$, module: "openUrl", method: "open", url: options.url });
      if (!enqueue || enqueue.ok !== true) { pending.delete(requestId); reject(enqueue && enqueue.error ? enqueue.error : { code: "QUEUE_REJECTED" }); }
    });
  };
  return Object.freeze({ default: Object.freeze({ open, openUrl: open }), __dispatchResult: function(result) {
    const item = pending.get(result.requestId);
    if (!item) return false;
    pending.delete(result.requestId);
    if (result.status === "completed") item.resolve(result);
    else item.reject(result.error || { code: result.status });
    return true;
  } });
})()
)JS";
constexpr std::string_view kWebviewFacadeSource = R"JS(
(() => {
  let requestSequence = 900000;
  const pending = new Map();
  const open = function(options) {
    if (!options || typeof options.url !== "string" || !/^https?:\/\//.test(options.url) || typeof globalThis.$quickapp_current_surface_id$ !== "string") throw new TypeError("system.webview.open requires an http(s) URL and Surface context");
    const requestId = "req:j-" + (++requestSequence);
    return new Promise((resolve, reject) => {
      pending.set(requestId, { resolve, reject });
      const enqueue = $quickapp_runtime_v1_featureRequest$({ schemaVersion: 1, kind: "featureRequest", requestId, surfaceId: globalThis.$quickapp_current_surface_id$, module: "webview", method: "open", url: options.url });
      if (!enqueue || enqueue.ok !== true) { pending.delete(requestId); reject(enqueue && enqueue.error ? enqueue.error : { code: "QUEUE_REJECTED" }); }
    });
  };
  return Object.freeze({ default: Object.freeze({ open, openUrl: open }), __dispatchResult: function(result) {
    const item = pending.get(result.requestId);
    if (!item) return false;
    pending.delete(result.requestId);
    if (result.status === "completed") item.resolve(result);
    else item.reject(result.error || { code: result.status });
    return true;
  } });
})()
)JS";
constexpr std::string_view kTimerFacadeSource = R"JS(
(() => {
  let requestSequence = 400000;
  const pending = new Map();
  const active = new Map();
  const api = {
    start: function(options) {
      if (!options || typeof options !== "object" ||
          typeof options.callback !== "function" ||
          !Number.isFinite(options.delay) || options.delay < 1 ||
          (options.interval !== undefined &&
           (!Number.isFinite(options.interval) || options.interval < 0)) ||
          typeof globalThis.$quickapp_current_surface_id$ !== "string") {
        throw new TypeError("system.timer.start requires delay and callback");
      }
      const requestId = "req:j-" + (++requestSequence);
      pending.set(requestId, options.callback);
      const result = $quickapp_runtime_v1_startTimer$({
        schemaVersion: 1, kind: "timerStart", requestId,
        surfaceId: globalThis.$quickapp_current_surface_id$,
        delayMs: options.delay,
        periodMs: options.interval === undefined ? 0 : options.interval
      });
      if (!result || result.ok !== true) pending.delete(requestId);
      return { requestId, enqueue: result };
    },
    cancel: function(timerId) {
      if (typeof timerId !== "string" || !timerId.startsWith("tmr:") ||
          typeof globalThis.$quickapp_current_surface_id$ !== "string") {
        throw new TypeError("system.timer.cancel requires TimerId and Surface context");
      }
      return $quickapp_runtime_v1_cancelTimer$({
        schemaVersion: 1, kind: "timerCancel",
        requestId: "req:j-" + (++requestSequence),
        surfaceId: globalThis.$quickapp_current_surface_id$, timerId
      });
    }
  };
  return Object.freeze({
    default: Object.freeze(api),
    __dispatchStartResult: function(result) {
      const callback = pending.get(result.requestId);
      pending.delete(result.requestId);
      if (result.status === "completed" && result.timerId && callback) {
        active.set(result.timerId, callback);
      }
    },
    __dispatchCancelResult: function(result) {
      if (result.status === "completed") active.delete(result.timerId);
    },
    __dispatchFired: function(event) {
      const callback = active.get(event.timerId);
      if (callback) callback(event);
    }
  });
})()
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
  auto device = engine.evaluate(context, SourceUnit{"alpha-system-device", "quickapp://framework/system.device", std::string(kDeviceFacadeSource), SourceMode::Script});
  if (!device.ok()) return false;
  auto fetch = engine.evaluate(context, SourceUnit{"alpha-system-fetch", "quickapp://framework/system.fetch", std::string(kFetchFacadeSource), SourceMode::Script});
  if (!fetch.ok()) return false;
  auto file = engine.evaluate(context, SourceUnit{"alpha-system-file", "quickapp://framework/system.file", std::string(kFileFacadeSource), SourceMode::Script});
  if (!file.ok()) return false;
  auto openUrl = engine.evaluate(context, SourceUnit{"alpha-system-open-url", "quickapp://framework/system.openUrl", std::string(kOpenUrlFacadeSource), SourceMode::Script});
  if (!openUrl.ok()) return false;
  auto webview = engine.evaluate(context, SourceUnit{"alpha-system-webview", "quickapp://framework/system.webview", std::string(kWebviewFacadeSource), SourceMode::Script});
  if (!webview.ok()) return false;
  auto globals = engine.evaluate(context, SourceUnit{"alpha-global-alias", "quickapp://framework/global", "globalThis.global = globalThis;", SourceMode::Script});
  if (!globals.ok()) return false;
  engine_ = &engine;
  context_ = &context;
  routerFacade_ = std::move(facade).value();
  promptFacade_ = std::move(prompt).value();
  deviceFacade_ = std::move(device).value();
  fetchFacade_ = std::move(fetch).value();
  fileFacade_ = std::move(file).value();
  openUrlFacade_ = std::move(openUrl).value();
  webviewFacade_ = std::move(webview).value();
  running_ = true;
  return true;
}

Result<JsValueRef, module::ModuleError>
StaticFacadeCatalog::resolveOnExecutor(std::string_view moduleId) noexcept {
  if (!running_ || !engine_ || !context_ ||
       (moduleId != kRouterModule && moduleId != kPromptModule &&
       moduleId != kDeviceModule && moduleId != kTimerModule &&
       moduleId != kFetchModule && moduleId != kFileModule &&
       moduleId != kOpenUrlModule && moduleId != kWebviewModule)) {
    return Result<JsValueRef, module::ModuleError>::failure(
        failure(module::ModuleErrorCode::ModuleAbiUnsupported,
                "Framework module is not in the Alpha static facade catalog"));
  }
  const auto &facade = moduleId == kRouterModule ? routerFacade_ :
                       moduleId == kPromptModule ? promptFacade_ :
                       moduleId == kDeviceModule ? deviceFacade_ :
                       moduleId == kFetchModule ? fetchFacade_ : moduleId == kFileModule ? fileFacade_ : moduleId == kOpenUrlModule ? openUrlFacade_ : moduleId == kWebviewModule ? webviewFacade_ : timerFacade_;
  if (moduleId == kTimerModule && !timerFacade_.valid()) {
    auto timer = engine_->evaluate(
        *context_, SourceUnit{"alpha-system-timer",
                              "quickapp://framework/system.timer",
                              std::string(kTimerFacadeSource), SourceMode::Script});
    if (!timer.ok()) {
      return Result<JsValueRef, module::ModuleError>::failure(
          failure(timer.error().kind == EngineExceptionKind::OutOfMemory
                      ? module::ModuleErrorCode::OutOfMemory
                      : module::ModuleErrorCode::JsException,
                  timer.error().message));
    }
    timerFacade_ = std::move(timer).value();
  }
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

namespace {

bool dispatchTimerMethod(JsEnginePort& engine, const JsContextRef& context,
                         JsValueRef& facade, std::string_view method,
                         RuntimeValue value) noexcept {
  auto function = engine.getProperty(context, facade, method);
  if (!function.ok()) return false;
  auto argument = engine.fromRuntimeValue(context, value);
  auto callable = engine.isCallable(context, function.value());
  if (!argument.ok() || !callable.ok() || !callable.value()) return false;
  std::array<JsValueRef, 1> args{std::move(argument).value()};
  auto result = engine.call(context, function.value(), facade,
                            std::span<const JsValueRef>(args.data(), args.size()));
  return result.ok();
}

}  // namespace

bool StaticFacadeCatalog::dispatchTimerStartResultOnExecutor(
    const abi::TimerStartResult& value) noexcept {
  if (!running_ || !engine_ || !context_) return false;
  RuntimeValue::Object object{{"requestId", RuntimeValue(value.requestId)},
                              {"surfaceId", RuntimeValue(value.surfaceId)},
                              {"status", RuntimeValue(value.status)}};
  if (value.timerId) object.emplace("timerId", RuntimeValue(*value.timerId));
  return dispatchTimerMethod(*engine_, *context_, timerFacade_,
                             "__dispatchStartResult", RuntimeValue(std::move(object)));
}

bool StaticFacadeCatalog::dispatchTimerCancelResultOnExecutor(
    const abi::TimerCancelResult& value) noexcept {
  if (!running_ || !engine_ || !context_) return false;
  return dispatchTimerMethod(
      *engine_, *context_, timerFacade_, "__dispatchCancelResult",
      RuntimeValue(RuntimeValue::Object{
          {"requestId", RuntimeValue(value.requestId)},
          {"surfaceId", RuntimeValue(value.surfaceId)},
          {"status", RuntimeValue(value.status)},
          {"timerId", RuntimeValue(value.timerId)}}));
}

bool StaticFacadeCatalog::dispatchTimerFiredOnExecutor(
    const abi::TimerFired& value) noexcept {
  if (!running_ || !engine_ || !context_) return false;
  return dispatchTimerMethod(
      *engine_, *context_, timerFacade_, "__dispatchFired",
      RuntimeValue(RuntimeValue::Object{
          {"surfaceId", RuntimeValue(value.surfaceId)},
          {"timerId", RuntimeValue(value.timerId)},
          {"sequence", RuntimeValue(static_cast<double>(value.sequence))},
          {"missedPeriods", RuntimeValue(static_cast<double>(value.missedPeriods))}}));
}

bool StaticFacadeCatalog::dispatchFeatureResultOnExecutor(
    const abi::FeatureResult& value) noexcept {
  if (!running_ || !engine_ || !context_) return false;
  RuntimeValue::Object object{{"requestId", RuntimeValue(value.requestId)},
                              {"surfaceId", RuntimeValue(value.surfaceId)},
                              {"status", RuntimeValue(value.status)}};
  if (value.confirmed) object.emplace("confirmed", RuntimeValue(*value.confirmed));
  if (value.httpStatus) object.emplace("httpStatus", RuntimeValue(static_cast<double>(*value.httpStatus)));
  if (value.responseBody) object.emplace("responseBody", RuntimeValue(*value.responseBody));
  if (value.responseIsJson) object.emplace("responseIsJson", RuntimeValue(*value.responseIsJson));
  if (value.fileData) object.emplace("fileData", RuntimeValue(*value.fileData));
  if (value.fileExists) object.emplace("fileExists", RuntimeValue(*value.fileExists));
  if (value.error) {
    object.emplace("error", RuntimeValue(RuntimeValue::Object{
        {"code", RuntimeValue(value.error->code)},
        {"message", RuntimeValue(value.error->message)},
        {"retryable", RuntimeValue(value.error->retryable)}}));
  }
  const auto payload = RuntimeValue(std::move(object));
  bool dispatched = dispatchTimerMethod(*engine_, *context_, promptFacade_,
                                        "__dispatchResult", payload);
  dispatched = dispatchTimerMethod(*engine_, *context_, fetchFacade_,
                                   "__dispatchResult", payload) || dispatched;
  dispatched = dispatchTimerMethod(*engine_, *context_, fileFacade_,
                                   "__dispatchResult", payload) || dispatched;
  dispatched = dispatchTimerMethod(*engine_, *context_, openUrlFacade_,
                                   "__dispatchResult", payload) || dispatched;
  dispatched = dispatchTimerMethod(*engine_, *context_, webviewFacade_,
                                   "__dispatchResult", payload) || dispatched;
  return dispatched;
}

void StaticFacadeCatalog::stopOnExecutor() noexcept {
  routerFacade_.reset();
  promptFacade_.reset();
  deviceFacade_.reset();
  timerFacade_.reset();
  fetchFacade_.reset();
  fileFacade_.reset();
  openUrlFacade_.reset();
  webviewFacade_.reset();
  engine_ = nullptr;
  context_ = nullptr;
  running_ = false;
}

} // namespace quickapp::js::framework
