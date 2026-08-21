# JS-S03 Module ABI 与 Loader 实现证据

## 目录

- [1. 结论](#1-结论)
- [2. 版本与边界](#2-版本与边界)
- [3. 实现合同](#3-实现合同)
- [4. 可复现命令](#4-可复现命令)
- [5. 构建结果](#5-构建结果)
- [6. 验收映射](#6-验收映射)
- [7. 资源与摘要](#7-资源与摘要)

## 1. 结论

JS-S03 已实现并提交 `READY_FOR_REVIEW`。实现范围是 verified immutable bytes 到 Module Definition Cache 的单一 Loader：Bundle 只在 JS Executor 上执行，`$app_define$`、`$app_bootstrap$`、`$app_require$` 通过 JS-S01 Engine Port 工作；App/Shared/Page cache、Shared lazy factory、Page Surface lease、Definition 校验、确定性失败缓存、completion outbox 和 teardown 已落地。

JS-S04、Binding、Event、Render、Capability 和 Platform 均未实现。S03 不读取 RPK、文件、源码目录或 Page IR。

## 2. 版本与边界

| 项目 | 值 |
|---|---|
| 证据日期 | 2026-08-18 |
| QuickJS | `2026-06-04` |
| OS | Darwin 25.5.0, arm64 |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.4.2 |
| C++ | C++20, extensions off |
| 编译告警 | `-Wall -Wextra -Wpedantic -Werror` |
| 源码摘要 | `evidence/js-s03/source-manifest.sha256` |

## 3. 实现合同

| 合同 | 实现位置 | 证据 |
|---|---|---|
| immutable bytes、长度和 SHA-256 二次校验 | `src/module/module_loader.cpp` | App 成功、长度错误、SHA 错误测试 |
| `$app_define$` | `invokeDefine/parseDefine` | define cardinality、moduleId、dependency 顺序、callable 校验；factory 参数为 `require/module/exports` |
| `$app_bootstrap$` | `invokeBootstrap/parseBootstrap` | App/Page expected metadata、Shared 禁止 bootstrap |
| `$app_require$` | `invokeRequire/requireModule` | declared dependency、Shared lazy single evaluation；Framework builtin 只交给 `FrameworkModuleResolverPort`；native callback 不递归进入 Engine |
| Definition shape | validator Script + `validateDefinition` | App/Page own data property、factory callable、Page binding/handler ID 集合 |
| cache 与 lease | `entries_`、`surfaces_`、`PageModuleLease` | 重复 App、Shared dependency、双 Surface Page lease |
| 失败语义 | `cacheDeterministicFailure` | ABI failure cache；OOM/queue overflow rollback 和新 RequestId retry |
| completion | `complete`、`outbox_`、`retryCompletionsOnExecutor` | Core backpressure 不重跑 Bundle |
| teardown | `closeSurfaceOnExecutor`、`releaseAllOnExecutor` | Page lease、bytes、entry、outbox 归零 |
| observation | `module.load.started/completed/failed` | 复用 JS-S01 ObservationEmitter，Noop 可关闭 |

依赖求值在 factory 调用前递归完成。原因是 QuickJS native callback 期间禁止递归调用 Engine；因此 `$app_require$` 只返回已 Loaded 的 immutable exports 引用，不在 native callback 内执行另一个 factory。

## 4. 可复现命令

```bash
cmake -S . -B build/js-s03-debug -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/js-s03-debug --parallel 8
ctest --test-dir build/js-s03-debug --output-on-failure

cmake -S . -B build/js-s03-release -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/js-s03-release --parallel 8
ctest --test-dir build/js-s03-release --output-on-failure

cmake -S . -B build/js-s03-asan-ubsan -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON -DQUICKAPP_JS_ENABLE_ASAN=ON -DQUICKAPP_JS_ENABLE_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/js-s03-asan-ubsan --parallel 8
ctest --test-dir build/js-s03-asan-ubsan --output-on-failure

cmake -S . -B build/js-s03-tsan -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON -DQUICKAPP_JS_ENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/js-s03-tsan --parallel 8
ctest --test-dir build/js-s03-tsan --output-on-failure

cmake -S . -B build/js-s03-api-only -DQUICKAPP_JS_BUILD_TESTS=OFF -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/js-s03-api-only --parallel 8
```

## 5. 构建结果

| 配置 | 结果 |
|---|---|
| Debug | CTest 7/7 PASS |
| Release | CTest 7/7 PASS |
| ASan + UBSan | CTest 7/7 PASS，无 sanitizer 报告 |
| TSan | CTest 7/7 PASS，无 data race 报告 |
| API-only | `quickapp_js_module_loader` 在无 QuickJS Provider 下构建通过 |
| Boundary scan | S01、S02、S03 均 PASS |

S03 合同测试包含：QuickJS App load/cache、Fake/QuickJS common App load、immutable bytes、SHA/length/integrity failure、Definition failure cache、Shared lazy require、Page 双 Surface lease、completion overflow retry、OOM retry、entry capacity overflow、lease/handle 查询、bytes/entry/outbox teardown。

## 6. 验收映射

| 验收 | 证据 |
|---|---|
| A01-A07 | `verifyAppLoadAndCache`、`verifyIntegrityAndAbiFailure`、`js_s03_boundary_scan` |
| A08-A17 | QuickJS App load、Fake scripted `$app_define$/$app_bootstrap$`、Definition validator；S03 source boundary |
| A18-A28 | `verifySharedAndPageScopes`、Shared lazy single evaluation、Page scope isolation、failure cache |
| A29-A35 | App Definition validator、Page `bindingEvaluators/handlerMethods` ID set、`PageModuleLease` |
| A36-A40 | completion port、duplicate cache hit、outbox retry、Surface lease close |
| A41-A46 | `verifyCompletionRetryAndTeardown`、OOM/queue limit test、bytes weak ownership、stop resource snapshot |
| A47-A50 | S03 boundary scan、Debug/Release/ASan/UBSan/TSan/API-only matrix、source manifest |

逐条验收的最终裁决仍由定向校审依据 `acceptance.md` 执行；本文件记录的是当前实现和可复现证据，不把未实现的 S04 行为计入 S03。

## 7. 资源与摘要

- 成功 load 后 cache 不保存 Bundle bytes；`retainedBytes=0`，bytes weak owner 可释放。
- Surface close 只释放对应 Page lease；另一个 Surface 的 lease 仍可查询。
- stop 后 `liveEntries`、`livePageLeases`、`activeLoads`、`retainedBytes`、`pendingCompletions` 全为 0。
- `ModuleLoader` public/source boundary 不包含 QuickJS public type、JNI、UIKit、LVGL、RPK、Page IR 或文件 I/O；只定义 Framework builtin resolver port，不实现 Capability Provider。
- Fake Engine 为测试专用脚本驱动；生产 S03 只依赖 `JsEnginePort`，不依赖 Fake 或 QuickJS 类型。
