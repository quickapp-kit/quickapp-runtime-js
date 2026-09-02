# QuickApp Runtime JS

Pure JavaScript Framework boundary for QuickApp Kit.

## Status

This repository no longer stores or compiles C/C++ Runtime code. The QuickJS adapter, `JsEngineService`, event loops, module loader, VM host, native binding, and Runtime ABI now live in `quickapp-runtime-core/runtime/js`.

The reactive Framework is not yet a standalone, versioned Bundle consumed by the Runtime. Its real implementation is still emitted inline per page by Toolkit's `js-module-emitter.ts`: Proxy/Watcher, dirty bindings, block reconciliation, render intent, and microtask flushing. `framework/source.json` records that fact, and the boundary test verifies the real emitter directly.

## Responsibilities

- Record the single source of current JavaScript Framework behavior.
- Reject C/C++ source files in this repository.
- Verify that Toolkit still emits the active reactive path.
- Host the future versioned Framework Bundle, JavaScript behavior tests, and Bundle build.

## Verify

```bash
npm test

cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Open Work

- Extract Toolkit's page-inline runtime into a versioned standalone Framework Bundle.
- Make Toolkit reference that Bundle while preserving the RPK contract.
- Replace the C++ compatibility facade with the complete typed JavaScript Feature Facade.

This migration changes code ownership only; it does not change RPK, Runtime ABI, or runtime behavior.

## License

[MIT](LICENSE)
