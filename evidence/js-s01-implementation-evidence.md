# JS-S01 实现证据

## 目录

- [1. 结论](#1-结论)
- [2. 版本与环境](#2-版本与环境)
- [3. 构建矩阵](#3-构建矩阵)
- [4. 合同测试](#4-合同测试)
- [5. 验收映射](#5-验收映射)
- [6. Composition 与依赖边界](#6-composition-与依赖边界)
- [7. 资源与线程证据](#7-资源与线程证据)
- [8. 范围检查](#8-范围检查)

## 1. 结论

JS-S01 已完成实现并满足 `A01..A43`：Fake 与 QuickJS 运行同一 Engine Contract Suite；QuickJS、Executor、Observation、生命周期、异常、背压和确定销毁均有可复现证据；生产 composition 只选择 `engine.quickjs`。

本实现没有启动 JS-S02，也没有加入 Runtime ABI、Module Loader、VM、Binding、Render、Handler 或 Platform Host。

## 2. 版本与环境

| 项目 | 事实 |
|---|---|
| 代码版本 | [source-manifest.sha256](./source-manifest.sha256)；当前代码目录无独立 Git 元数据，因此以逐文件 SHA-256 固定版本 |
| QuickJS | vendored `2026-06-04`；Provider descriptor 与构建读取的 `VERSION` 一致 |
| 系统 | macOS 26.5.1, arm64 |
| Xcode | 26.6, build 17F113 |
| C/C++ | Apple Clang 21.0.0 |
| CMake | 4.4.2 |
| C++ 标准 | C++20，无扩展 |

## 3. 构建矩阵

| 配置 | 关键参数 | 结果 |
|---|---|---|
| Debug | `QUICKAPP_JS_BUILD_TESTS=ON`, `CMAKE_BUILD_TYPE=Debug` | CTest `3/3 PASS` |
| ASan + UBSan | `QUICKAPP_JS_ENABLE_ASAN=ON`, `QUICKAPP_JS_ENABLE_UBSAN=ON`, `RelWithDebInfo` | CTest `3/3 PASS` |
| TSan | `QUICKAPP_JS_ENABLE_TSAN=ON`, `RelWithDebInfo` | CTest `3/3 PASS` |
| API-only | `BUILD_TESTS=OFF`, `BUILD_QUICKJS_PROVIDER=OFF`, QuickJS path=`/nonexistent` | Engine API + Executor 独立构建成功 |

三套 CTest 均执行：

1. `js_s01_contract_tests`
2. `js_s01_quickjs_composition_probe`
3. `js_s01_boundary_scan`

## 4. 合同测试

`js_s01_contract_tests` 共 `17/17 PASS`：

| 测试 | 核心证明 |
|---|---|
| `EngineContractSuite` | Fake/QuickJS descriptor、Context、Value、eval/call/property、Native Binding、microtask、GC、异常语义一致 |
| `ContextLeakReconciliation` | 带活 Value 销毁先清理 Context，再返回确定失败；Value 随 Context 失效 |
| `QuickJsPureDataRejection` | undefined/function/Symbol/BigInt/NaN/Infinity/cycle/getter/Proxy/非 plain prototype 拒绝 |
| `NativeFailureIsolation` | Native C++ 异常不穿透，后续 JS operation 正常 |
| `CrossServiceRejection` | wrong Service/Context 在触碰引擎内存前失败 |
| `NativeRuntimeErrorCode` | JS Error 保留 typed error code，且不会污染后续普通异常分类 |
| `RepeatedQuickJsTeardown` | 50 轮 Engine/Context/Value 创建与销毁 |
| `QuickJsHeapLimit` | QuickJS heap limit 触发 `OutOfMemory` 和 OOM callback |
| `ManualExecutor` | 有界队列、overflow、唯一 continuation、取消和 teardown barrier |
| `ConcurrentExecutorFifo` | 4 producers/200 tasks；acceptance sequence 与消费顺序一致 |
| `ObservationContract` | `noexcept`、Sink admission、run-relative safe integer、run rotation |
| `ServiceCreationFailuresAndOomObservation` | Engine/Context 创建失败反向清理并产生 `runtime.oom` |
| `ObservationBehaviorEquivalence` | Noop、正常、容量满、丢样、关闭的业务结果与 task sequence 一致 |
| `ServiceQueueOverflowObservation` | 只拒绝新任务并产生 `queue.overflow` |
| `ServiceLifecycleAndMicrotaskContinuation` | start/stop、budget、continuation 去重和普通任务公平性 |
| `ServiceAbiMismatch` | descriptor mismatch 在执行 source 前停止 |
| `UnrecoverableEngineFailureStopsService` | `Terminated` 使 Service failed、拒绝后续任务并确定销毁 |

## 5. 验收映射

| ID | 证据 | 结果 |
|---|---|---|
| A01 | EngineContractSuite + composition probe | PASS |
| A02 | EngineContractSuite 正常/重复 destroy | PASS |
| A03 | EngineContractSuite `value/sum/thrower` | PASS |
| A04 | EngineContractSuite global/get/set/isCallable | PASS |
| A05 | EngineContractSuite retain 后释放原引用 | PASS |
| A06 | EngineContractSuite nested RuntimeValue round-trip | PASS |
| A07 | EngineContractSuite native echo | PASS |
| A08 | EngineContractSuite + ManualExecutor | PASS |
| A09 | EngineContractSuite GC/memory snapshot | PASS |
| A10 | ServiceAbiMismatch | PASS |
| A11 | CrossServiceRejection | PASS |
| A12 | EngineContractSuite wrong-thread memory operation | PASS |
| A13 | EngineContractSuite released Value | PASS |
| A14 | ContextLeakReconciliation | PASS |
| A15 | EngineContractSuite non-callable 后恢复 | PASS |
| A16 | EngineContractSuite limits/unsafe number + QuickJsPureDataRejection | PASS |
| A17 | QuickJsPureDataRejection getter/Proxy | PASS |
| A18 | EngineContractSuite duplicate/unbound native function | PASS |
| A19 | ManualExecutor + ServiceQueueOverflowObservation | PASS |
| A20 | ObservationBehaviorEquivalence | PASS |
| A21 | ServiceLifecycleAndMicrotaskContinuation | PASS |
| A22 | ServiceLifecycleAndMicrotaskContinuation | PASS |
| A23 | ServiceCreationFailuresAndOomObservation | PASS |
| A24 | ServiceCreationFailuresAndOomObservation | PASS |
| A25 | ServiceLifecycleAndMicrotaskContinuation | PASS |
| A26 | ManualExecutor cancellation + unique teardown | PASS |
| A27 | Service lifecycle post-after-stop | PASS |
| A28 | EngineContractSuite native echo | PASS |
| A29 | NativeRuntimeErrorCode | PASS |
| A30 | NativeFailureIsolation | PASS |
| A31 | EngineContractSuite retained argument | PASS |
| A32 | EngineContractSuite unbind + sanitizer teardown | PASS |
| A33 | EngineContractSuite syntax failure 后恢复 | PASS |
| A34 | EngineContractSuite runtime failure 后恢复 | PASS |
| A35 | EngineContractSuite function throw 后恢复 | PASS |
| A36 | QuickJsHeapLimit + OOM Observation | PASS |
| A37 | UnrecoverableEngineFailureStopsService | PASS |
| A38 | TraceSinkRegistration 拒绝 MayBlock/MayReenter；未执行真实违约行为 | PASS |
| A39 | API-only 无 QuickJS 路径构建 + boundary scan | PASS |
| A40 | runtime-composition + descriptor probe + production symbol scan | PASS |
| A41 | Fake 仅在 tests block 构建；production probe 无 Fake symbol | PASS |
| A42 | CMake 单 Provider 选择 + manifest 单 Engine 语义检查 | PASS |
| A43 | 公共 Runtime Composition Schema + 跨字段语义检查 | PASS |

## 6. Composition 与依赖边界

- [runtime-composition.json](./runtime-composition.json) 通过公共 `runtime-composition.schema.json`。
- 跨字段检查确认：moduleId 唯一、仅一个 `category=engine`、Engine module 与 `jsEngine.moduleId` 一致、`runtime.js-framework` 恰好一次。
- composition probe 输出：`quickjs 2026-06-04 quickapp-kit-js-engine-v1 engine.quickjs`。
- `nm` 扫描 production probe 不包含 `FakeEngine` 或 `engine.fake`。
- public/common 源码扫描不包含 QuickJS raw type/header，也不包含 JNI、UIKit、LVGL 或 libuv。
- API-only 构建在 QuickJS 路径不存在时成功，证明 API/Executor 不依赖具体 Engine SDK。

## 7. 资源与线程证据

- Provider registry 在 Context 销毁前释放 Value 与 Native Binding；存在活资源时完成清理并返回确定失败。
- QuickJS 每轮执行 `Value -> Context -> Runtime` 释放；50 轮 teardown 在 ASan/UBSan 下无 leak、UAF、double free 或越界报告。
- Executor quiescing 取消未执行 normal/continuation，运行唯一 barrier；析构要求 queue 为空且状态为 stopped。
- TSan 覆盖并发 producer、Service 状态、Observation 与跨线程 Value release，无 data race 报告。
- shutdown 后 Context/Value 失效，pending task 和 pending microtask continuation 均为零；composition probe 正常退出。

## 8. 范围检查

源码扫描未发现 VNode、RenderTransaction、Binding flush、Runtime ABI、Module Loader 或 Platform Host 实现。Observation 只产生 `runtime.counter.sampled`、`queue.overflow`、`runtime.oom` 三类 S01 事实。
