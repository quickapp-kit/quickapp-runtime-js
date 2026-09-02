# QuickApp Runtime JS

QuickApp Kit 的纯 JS Framework 代码边界。

## 结论

本仓库不再编译或保存 C/C++ Runtime。QuickJS Adapter、JsEngineService、EventLoop、Module Loader、VM Host、Native Binding 和 Runtime ABI 已归入 `quickapp-runtime-core/runtime/js`。

当前响应式 Framework 尚未形成可被 Runtime 独立加载的版本化 Bundle。真实实现仍由 Toolkit 的 `js-module-emitter.ts` 按页注入，包含 Proxy/Watcher、Dirty Binding、Block reconcile、RenderIntent 和 microtask flush。`framework/source.json` 固化这个事实，边界测试会直接校验真实生成器，不能仅凭移除 C++ 宣称独立 Bundle 已完成。

## 当前职责

- 记录纯 JS Framework 的唯一源码来源和目标语义；
- 校验仓库不含 C/C++ 源码；
- 校验 Toolkit 生成器仍包含现行响应式主链；
- 为后续版本化 Framework Bundle、JS 行为测试和 Bundle 构建保留入口。

## 验证

```bash
npm test

cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## 未完成

- 将 Toolkit 按页注入逻辑抽成版本化、可独立加载的 Framework Bundle；
- 让 Toolkit 引用该 Bundle，同时保持 RPK 合同兼容；
- 用纯 JS typed Feature Facade 完整替代当前 C++ 兼容 Host。

本轮只做职责归位，不改变 RPK、Runtime ABI 或运行行为。

## 许可证

[MIT](LICENSE)
