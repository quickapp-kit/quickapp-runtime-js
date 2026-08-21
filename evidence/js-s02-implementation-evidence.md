# JS-S02 Runtime ABI Client Implementation Evidence

## 目录

- [1. 结论](#1-结论)
- [2. 版本与环境](#2-版本与环境)
- [3. 实现边界](#3-实现边界)
- [4. 可复现命令](#4-可复现命令)
- [5. 构建矩阵](#5-构建矩阵)
- [6. A01-A50 映射](#6-a01-a50-映射)
- [7. 资源与依赖证据](#7-资源与依赖证据)

## 1. 结论

JS-S02 T01-T09 已实现，typed message model 与 ModuleBundle immutable byte storage 定向返修已完成，A01-A50 已由合同测试、sanitizer、API-only 构建和边界扫描覆盖。Fake Engine 与 QuickJS 运行同一 ABI 合同；未实现 JS-S03 或任何 Module/VM/Binding/Render/Handler/Platform 业务。

## 2. 版本与环境

| 项目 | 值 |
|---|---|
| 证据日期 | 2026-08-18 |
| 代码版本 | `evidence/js-s02/source-manifest.sha256` |
| Runtime ABI | `quickapp-kit-runtime-v1` / `schemaVersion=1` |
| QuickJS | `2026-06-04` |
| OS | macOS 26.5.1 (25F80), arm64 |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.4.2 |
| C++ | C++20, extensions off, `-Wall -Wextra -Wpedantic -Werror` |

## 3. 实现边界

- `quickapp_js_runtime_abi` 提供 13 个 outbound concrete struct、16 个 inbound concrete struct 和两个 closed `std::variant`；每条消息及其固定嵌套对象均为具名成员。
- decoder 是字符串字段解释的终点；`CoreIngressPort`、callback validator 和 typed consumer 只读取具名成员。`RuntimeValue` 仅保留在合同明确动态的 `params`、事件 `payload` 叶子。
- Fake Core 逐类断言 13 种 outbound 的成员与类型；16 种 callback slot 逐类读取具名成员。非空 Binding、Block、Handler 和 Render Operation 有独立提取测试。
- `PendingRecord` 只有 `key`、`expectedResultKind`、`owner`、`ownerGeneration`。
- JS-origin ID 只作为已存在的 `req:j-*` 输入被校验；生产代码无 allocator、ID Native Function 或 C++ ID 服务。
- `ModuleBundle.bytes` 进程内固定为 `shared_ptr<const vector<uint8_t>>`；`bytesBase64` 仅存在于 JSON fixture/Schema 边界，S02 不做 base64 编解码。
- accepted callback 只读共享同一 byte storage；rejected、queue overflow、terminal consumer delivery、Surface generation cancellation 和 App teardown cancellation 均由合同测试用 `weak_ptr` 证明释放。
- `TestJsRequestIdAllocator` 只存在于合同测试，验证 A/B/A 共享序列 `req:j-1/2/3`。
- Observation 只复用 JS-S01 emitter 和公共 `bridge.request.*` / `queue.overflow` marker；无额外队列、Sink 或 I/O。

## 4. 可复现命令

```bash
cmake -S . -B build/js-s02-debug -DCMAKE_BUILD_TYPE=Debug -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON
cmake --build build/js-s02-debug --parallel
ctest --test-dir build/js-s02-debug --output-on-failure

cmake -S . -B build/js-s02-release -DCMAKE_BUILD_TYPE=Release -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON
cmake --build build/js-s02-release --parallel
ctest --test-dir build/js-s02-release --output-on-failure

cmake -S . -B build/js-s02-asan-ubsan -DCMAKE_BUILD_TYPE=Debug -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON -DQUICKAPP_JS_ENABLE_ASAN=ON -DQUICKAPP_JS_ENABLE_UBSAN=ON
cmake --build build/js-s02-asan-ubsan --parallel
ctest --test-dir build/js-s02-asan-ubsan --output-on-failure

cmake -S . -B build/js-s02-tsan -DCMAKE_BUILD_TYPE=Debug -DQUICKAPP_JS_BUILD_TESTS=ON -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=ON -DQUICKAPP_JS_ENABLE_TSAN=ON
cmake --build build/js-s02-tsan --parallel
ctest --test-dir build/js-s02-tsan --output-on-failure

cmake -S . -B build/js-s02-api-only -DCMAKE_BUILD_TYPE=Release -DQUICKAPP_JS_BUILD_TESTS=OFF -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=OFF
cmake --build build/js-s02-api-only --parallel
```

## 5. 构建矩阵

| 配置 | 结果 |
|---|---|
| Debug | CTest 5/5 PASS |
| Release | CTest 5/5 PASS |
| ASan + UBSan | CTest 5/5 PASS；无 sanitizer 报告 |
| TSan | CTest 5/5 PASS；无 data race 报告 |
| API-only | `quickapp_js_runtime_abi` 构建通过；未链接 QuickJS Provider |
| Boundary scan | JS-S01 + JS-S02 均 PASS |

JS-S02 合同程序通过的场景组：allocator fixture、cross-field codec、identity failure、partial binding rollback、callback overflow、immutable ModuleBundle byte ownership、close race/same-thread queue、stop with pending work、Noop/Recording equivalence、QuickJS strict values、Fake common ABI、QuickJS common ABI。

## 6. A01-A50 映射

| ID | 证据 |
|---|---|
| A01 | Fake/QuickJS common ABI 启动后 `liveNativeEntries=14`。 |
| A02 | `verifyIdentityFailure`：错误 identity 返回 unsupported，0 binding/correlation。 |
| A03 | common ABI 的 `schemaVersion=2` 拒绝，后续消息继续成功。 |
| A04 | `verifyPartialBindingRollback` 预占固定 binding，重复注册启动失败。 |
| A05 | 同一测试在中间入口失败后反向解绑已注册 token，S02 entry 归零。 |
| A06 | Catalog 固定 13 个消息入口加一个 supports；supports 不 post、不建 correlation。 |
| A07 | `runAbiContractSuite` 分别使用 Fake 和 QuickJS Provider。 |
| A08 | common ABI 逐个提交 13 个 outbound concrete struct；Fake Core 直接断言 `templateId`、`revision`、`templateHandlerId` 等具名成员，非空嵌套对象另有提取测试。 |
| A09 | JS-S01 RuntimeValue 回归测试 + JS-S02 Fake/QuickJS codec round-trip。 |
| A10 | 全部合法 Native call 精确返回 `{ok:true}`。 |
| A11 | overflow/OOM/closed/invalid 精确返回 `{ok:false,error}`。 |
| A12 | `verifyCodecCrossFields` 验证 optional 缺失可接受、显式 null 不被当作缺失。 |
| A13 | required missing 与 unknown field 均拒绝，Core 未被调用。 |
| A14 | wrong kind、错误 RequestId 分区和 leading-zero ID 均拒绝。 |
| A15 | app VM failure 使用 page phase 被拒绝，合法 `onCreate` 被接受。 |
| A16 | `verifyQuickJsStrictValues` 覆盖 undefined、NaN 和不可表示值。 |
| A17 | QuickJS getter/Proxy 均拒绝，getter/trap 标志保持 false。 |
| A18 | unknown RuntimeError code 的 callback admission 被拒绝。 |
| A19 | JS-S01 native throw 回归仍通过；S02 Native entry 全部 noexcept catch，不穿透。 |
| A20 | common ABI 验证 accepted 后 provisional correlation 变 active。 |
| A21 | Fake Core 注入 `QUEUE_OVERFLOW`，provisional correlation 原子撤销。 |
| A22 | Fake Core 注入 closed，返回 terminal error 且无 correlation 残留。 |
| A23 | 10 条 correlation 达上限后新请求拒绝，Core post 数不增长。 |
| A24 | test-only A/B/A 共享 allocator 依次产生 `req:j-1/2/3`。 |
| A25 | 九类 RequestId Result 匹配后先删除 correlation，再单次投递 slot。 |
| A26 | `txn:1` Render Result 按 TransactionId 匹配并单次投递。 |
| A27 | duplicate/unknown Result 被丢弃，consumer 次数不增加。 |
| A28 | `req:j-7` 用错误 Result kind 投递时不消费合法 correlation。 |
| A29 | 10 个 Result 逆序投递，各自只消费对应 correlation。 |
| A30 | module/VM/lifecycle completion 原样使用 Core `req:*`，不建 correlation。 |
| A31 | callback slot 内断言 `executor.isOnExecutor()`。 |
| A32 | AppContext `pkg/pkg-one/pkg-two` 按 accepted FIFO 到达。 |
| A33 | `verifyModuleBundleByteOwnership`：callback queue overflow 后 Module bytes weak owner 立即释放，既有任务保留。 |
| A34 | consumer token 注销后 callback 与其 immutable bytes 被释放，不调用旧 consumer。 |
| A35 | callback 是闭合具体类型，无法携带 unknown field；具名 `status` 被改为非法值时在入队前拒绝。 |
| A36 | close task 排在已 admission callback 前时，generation recheck 丢弃 callback，并释放 Surface Module bytes。 |
| A37 | 从 JS Executor 内调用 `postCallback`，当前栈 consumer 次数仍为 0，后续队列才执行。 |
| A38 | Surface close 先禁 admission，再清该 Surface correlation。 |
| A39 | close 后 Result/Event 返回 Surface not found，不触达 consumer。 |
| A40 | 重复 Surface close 两次均成功且无重复释放。 |
| A41 | normal stop 反向解绑 14 entry，最终 stopped。 |
| A42 | `verifyModuleBundleByteOwnership` 与 `verifyStopWithPendingWork` 证明 App teardown cancellation 释放 queued Module bytes 和 correlation。 |
| A43 | identity/start failure 与 partial bind failure 后 stop 幂等，不产生第二套 Catalog。 |
| A44 | S02 作为 JS-S01 `upperLayerTeardown` 执行，先清 ABI 资源，再销毁 Context/Engine。 |
| A45 | stop 后五项资源计数全部为 0；ASan/UBSan/TSan 通过，terminal delivery 后 Module bytes weak owner 归零。 |
| A46 | `js_s02_boundary_scan`：ABI public/source 无 QuickJS、JNI、UIKit、LVGL 或后续业务实现。 |
| A47 | 边界扫描拒绝 `CoreMessage<...>`、`JsCallbackMessage<...>`、通用 `fields`/`payload`、`module/method/args` 和 JSON Bridge，并继续拒绝 allocator、completionToken 与业务 pending 字段。 |
| A48 | Recording Sink 收到公共 bridge marker；关联 ID 通过结构化 TraceEvent 字段传递。 |
| A49 | `verifyObservationEquivalence` 比较 Noop/Recording 的返回、Core post、correlation 和 stop 结果。 |
| A50 | codec、correlation、callback queue 均使用固定 limits；PendingRecord 四字段与 `ImmutableByteStorage` const 共享所有权由边界扫描锁定。 |

## 7. 资源与依赖证据

- stop 后：`liveNativeEntries=0`、`liveBridgeCorrelations=0`、`liveConsumerRegistrations=0`、`openSurfaceScopes=0`、`queuedAbiCallbacks=0`。
- API-only archive 的未解析符号扫描无 `JS_*`、QuickJS 或 Provider 符号。
- ABI public/source 扫描无 `JsRequestIdAllocator`、`completionToken`、Promise、Render snapshot、JSON parse/stringify 或平台头文件。
- 本轮实现未修改公共合同或公共 Schema；只修改 JS-S02 实现、合同测试、边界扫描、证据和 source manifest。
