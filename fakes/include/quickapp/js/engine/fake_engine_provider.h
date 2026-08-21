#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "quickapp/js/engine/js_engine_port.h"

namespace quickapp::js::testing {

enum class FakeFunctionBehavior {
  ReturnConfiguredValue,
  ReturnConfiguredData,
  EchoFirstArgument,
  SumArguments,
  SetModuleExportsFromConfiguredValue,
};

struct FakeNativeCallArgument {
  RuntimeValue value;
  bool useEvaluatedFunction{false};
};

struct FakeNativeCall {
  std::string globalName;
  std::vector<FakeNativeCallArgument> arguments;
};

struct FakeSourcePlan {
  RuntimeValue result;
  std::optional<EngineException> exception;
  bool callable{false};
  FakeFunctionBehavior functionBehavior{
      FakeFunctionBehavior::ReturnConfiguredValue};
  std::optional<EngineException> functionException;
  std::uint32_t microtasks{0};
  std::vector<std::string> callableProperties;
  std::vector<FakeNativeCall> nativeCalls;
};

struct FakeEngineOptions {
  bool failEngineCreate{false};
  bool failContextCreate{false};
};

class FakeEngineProvider final : public JsEngineProvider {
public:
  struct SharedData;

  explicit FakeEngineProvider(FakeEngineOptions options = {});

  void setPlan(std::string sourceId, FakeSourcePlan plan);
  [[nodiscard]] const std::vector<std::string> &operations() const noexcept;

  [[nodiscard]] JsEngineDescriptor describe() const noexcept override;
  [[nodiscard]] std::unique_ptr<JsEnginePort>
  create(const JsEngineConfig &config) noexcept override;

private:
  std::shared_ptr<SharedData> data_;
  FakeEngineOptions options_;
};

} // namespace quickapp::js::testing
