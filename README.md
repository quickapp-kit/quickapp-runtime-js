# QuickApp Runtime JS

JS engine integration layer for [QuickApp Kit](https://github.com/quickapp-kit). Bridges JavaScript execution to the C++ runtime core.

## What's here

This layer provides the JS execution environment that drives the runtime:

- **Engine API** — abstract JS engine interface (provider-agnostic)
- **QuickJS Provider** — concrete provider using vendored QuickJS
- **Module Loader** — JS module resolution and loading
- **Page Host** — page-level JS context management
- **VM Lifecycle** — JS VM creation, teardown, memory management
- **Binding** — native ↔ JS binding layer
- **Event** — JS event dispatch and handling
- **Render** — JS-driven render intent generation

## Requirements

- C++20 / C11 compiler
- CMake 3.24+
- Vendored QuickJS source (configurable via `QUICKAPP_JS_QUICKJS_SOURCE_DIR`)

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### Sanitizers

```bash
cmake -S . -B build-asan -G Ninja \
  -DQUICKAPP_JS_ENABLE_ASAN=ON -DQUICKAPP_JS_ENABLE_UBSAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DQUICKAPP_JS_ENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

## Project Structure

```
├── include/quickapp/js/    # Public headers
├── src/
│   ├── engine/             # Engine abstraction
│   ├── binding/            # Native ↔ JS binding
│   ├── module/             # Module loader
│   ├── page/               # Page host control
│   ├── vm/                 # VM lifecycle
│   ├── event/              # Event dispatch
│   ├── render/             # Render intent
│   ├── alpha/              # Alpha integration (initial render, binding, page stage)
│   ├── framework/          # JS framework facades
│   └── abi/                # ABI layer
├── providers/              # Engine providers (QuickJS)
├── fakes/                  # Test doubles
├── tests/                  # Contract tests
├── cmake/                  # Build utilities
└── tools/                  # Verification scripts
```

## Related

- [quickapp-runtime-core](https://github.com/quickapp-kit/quickapp-runtime-core) — C++ runtime kernel
- [quickapp-runtime-android](https://github.com/quickapp-kit/quickapp-runtime-android) — Android adapter
- [quickapp-runtime-lvgl](https://github.com/quickapp-kit/quickapp-runtime-lvgl) — LVGL adapter

## License

[MIT](LICENSE)
