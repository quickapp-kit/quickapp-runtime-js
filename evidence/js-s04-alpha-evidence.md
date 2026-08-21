# JS-S04 M1-Alpha Component Evidence

## 结论

本文件是 **JS Alpha 组件与装配门禁证据**，不是 Case 001 S1 通过证据。当前用真实 QuickJS 从合成 `VerifiedModule` immutable bytes 加载 App/Page Definition，分别创建 App VM 和 Surface Page VM，在单一 JS Executor 上完成初始化。职责已归位：S04 只编排 VM/Hook，Alpha S05 Binding Stage 计算初始值，Alpha S07 Initial Transaction Builder 通过既有 Runtime ABI Native Function 提交 `InstantiateTemplate`。

这不是完整 JS-S04、JS-S05 或 JS-S07 实现，也没有创建 Alpha 专用 Runtime。未实现完整 Reactive、Block、Event、Navigation、Capability 或 S2-S5；当前不能宣称真实 Case 001 或 LVGL/SDL 首屏已通过。

## 运行事实

| 项目 | 结果 |
|---|---|
| App Definition | 合成 Definition 的 `createAppVm` 成功调用一次 |
| Page Definition | 合成 Definition 的 `createPageVm` 成功调用一次 |
| VM 隔离 | App VM 1 个；Page VM 1 个，绑定 `srf:1` |
| Binding Stage | Alpha S05 evaluator `1 -> "Hello"`、`2 -> true` |
| Transaction Builder | Alpha S07 使用既有 `$quickapp_runtime_v1_instantiateTemplate$`，无新增 Bridge |
| Owner | `cmp:srf:1` |
| 线程 | Context、Definition、VM、evaluator、Native Function、teardown 均在 JS Executor |
| 释放 | Surface close 后 Page VM 归零；停止后 App/Page VM 均归零 |

## 可复现命令

```sh
cmake -S . -B build/js-s04-debug \
  -DQUICKAPP_JS_BUILD_TESTS=ON \
  -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/js-s04-debug --parallel 4
ctest --test-dir build/js-s04-debug --output-on-failure
```

Debug: `10/10 PASS`，包含 JS-S01/S02/S03 回归、S04 QuickJS 垂直测试、S04 boundary scan 和 Alpha S05/S07 所有权扫描。

```sh
cmake -S . -B build/js-s04-release \
  -DQUICKAPP_JS_BUILD_TESTS=ON \
  -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/js-s04-release --parallel 4
ctest --test-dir build/js-s04-release --output-on-failure
```

Release: `10/10 PASS`。

```sh
cmake -S . -B build/js-s04-asan-ubsan \
  -DQUICKAPP_JS_BUILD_TESTS=ON \
  -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON \
  -DQUICKAPP_JS_ENABLE_ASAN=ON \
  -DQUICKAPP_JS_ENABLE_UBSAN=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/js-s04-asan-ubsan --parallel 4
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 \
  ctest --test-dir build/js-s04-asan-ubsan --output-on-failure
```

ASan/UBSan: `10/10 PASS`。当前 macOS AppleClang 不支持 LeakSanitizer；资源释放由测试内的 VM/Page/Loader/Engine 快照和确定性 teardown 断言覆盖。

```sh
cmake -S . -B build/js-s04-tsan \
  -DQUICKAPP_JS_BUILD_TESTS=ON \
  -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON \
  -DQUICKAPP_JS_ENABLE_TSAN=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/js-s04-tsan --parallel 4
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/js-s04-tsan --output-on-failure
```

TSan: `10/10 PASS`。

```sh
cmake -S . -B build/js-s04-api-only \
  -DQUICKAPP_JS_BUILD_TESTS=OFF \
  -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/js-s04-api-only --parallel 4
```

API-only: `quickapp_js_vm_lifecycle` 及其依赖静态库构建通过。

## 边界与摘要

- `cmake/check_js_s04_boundaries.cmake` 拒绝 VNode、Render、Navigation、Capability、平台类型、文件 I/O 和 QuickJS 私有头泄漏。
- `cmake/check_js_alpha_stage_boundaries.cmake` 分别拒绝 S05 持有 Instantiate/RequestId 和 S07 持有 evaluator/ModuleLoader/普通 RenderTransaction。
- S04 只调用已有 Runtime ABI 的 `completeVmInitialization`；Alpha S07 才调用已有 `instantiateTemplate`，没有新增公共合同、Schema 或第二条 Bridge。
- `PageInitializationStagePort` 是 S04 与 Alpha S05/S07 的唯一 typed 编排边界；S04 源码不包含 evaluator、InstantiateTemplate 或 JS RequestId allocator。
- S03 的 Module Loader 继续只消费 immutable bytes；S04 不读取 RPK、源码、路径或 Page IR。
- `source-manifest.sha256` 覆盖本次源码、测试、边界脚本和既有实现证据，使用 `shasum -a 256 -c` 校验。

## 真实 Case 001 门禁

TK-S07 RPK 现已存在并由 Core 验证：

- RPK SHA-256：`6a8c0d1acc690e97594e4a625436485cb8c92f283f9b347e6a6123c693fa3141`。
- Core probe：`CORE_PACKAGE_LOADER_PASS package=com.example.case1 app=1 page=1 page_ir=page:/pages/Demo`。
- Core 已交付 App/Page immutable bytes 和 Page IR；JS 侧仍只接收前两者，不读取 RPK、路径或 Page IR。

真实 JS 装配仍未通过，原因是 TK-S07 当前 Bundle 与冻结 JS Module ABI 还有可复现的产物问题：

1. App 的 shared dependency 闭包包含 `@quickapp-kit/shared/helper/apis/index` 的自依赖，严格 S03 loader 按冻结规则判定循环依赖。
2. `system.router`、`system.prompt` 和 `require.context` 需要 Composition Root 提供已创建的 Framework facade；本 JS 端只提供 resolver port，不实现 Capability Provider。
3. Demo evaluator 使用自由变量 `title`，而不是冻结合同要求的 `this/scope` 可寻址值；Demo `onInit` 还依赖尚未授权的 `$page` Host Control facade。

因此本轮形成的是“Core verified input 已到位 + JS职责归位 + 真实 Bundle 装配阻塞”的正式证据，不能把合成测试升级为真实 Case 001 通过。待 Toolkit/Examples 按既有合同修正产物与 facade 装配后，继续验证：

```text
TK-S07 RPK
  -> verified App/Page Bundle bytes
  -> JS-S03 Module Loader
  -> App/Page VM
  -> real Page binding evaluator
  -> title/titleBar initial binding
  -> InstantiateTemplate
```

## 状态

`M1-Alpha JS职责归位 READY_FOR_REVIEW / REAL_RPK_ASSEMBLY_BLOCKED_BY_ARTIFACT_CONTRACT`。

完整 JS-S04 的 Lifecycle、Ledger、背压、Hook 顺序和强制 teardown 仍按分 Spec 保持后续范围，本证据不将 Alpha 子集冒充完整 S04。
