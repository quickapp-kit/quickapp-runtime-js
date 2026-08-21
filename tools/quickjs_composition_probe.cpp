#include <iostream>

#include "quickapp/js/engine/quickjs_engine_provider.h"

int main() {
  quickapp::js::QuickJsEngineProvider provider;
  const auto descriptor = provider.describe();
  if (descriptor.engineId != "quickjs" ||
      descriptor.moduleId != "engine.quickjs" ||
      descriptor.engineAbi != "quickapp-kit-js-engine-v1") {
    return 1;
  }
  std::cout << descriptor.engineId << ' ' << descriptor.engineVersion << ' '
            << descriptor.engineAbi << ' ' << descriptor.moduleId << '\n';
  return 0;
}
