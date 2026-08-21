# Alpha S1 Typed Facade Evidence

## 结论

Case 001 S1 所需 JS typed facade 组件已经实现并通过验证：页面 Bundle 可解析 `@app-module/system.router` 的静态 default export；Page VM 按 `SurfaceContext.hostCapabilities` 安装 `$page.setTitleBar/setMeta`；两项调用通过既有 typed Runtime ABI 向 Core 提交具名消息；随后完成 `onInit -> initial binding -> InstantiateTemplate`。

本证据是 QuickJS 合成 VerifiedModule 的组件证据，不宣称真实 RPK 到 LVGL/SDL 的整链路已经通过。

## 组件合同

| 组件 | 已验证结果 |
|---|---|
| Static Facade Catalog | 只接受 `@app-module/system.router`；返回含 `default.push` 的冻结 facade；未知模块拒绝 |
| Router S1 边界 | `push` 不发起路由请求；S1 未调用它 |
| Page VM 注入 | `setTitleBar`、`setMeta` 仅在对应 host capability 存在时安装 |
| Typed ABI | `onInit` 产生 `SetTitleBar{req:j-4,srf:1,Welcome}` 和 `SetMeta{req:j-5,srf:1,...}` |
| Initial Binding | evaluator 产生 `1 -> "Hello"`、`2 -> true` |
| Instantiate | 产生 `InstantiateTemplate{req:j-6,cmp:srf:1,...}` |
| 所有权 | S04 只调用 `PageVmSetupPort`；页面控制 adapter 拥有 facade 注入；Runtime ABI 拥有 admission/correlation |
| 释放 | Surface/VM 归零；页面控制 Native Binding 与 factory value 归零 |

## 验证结果

| 配置 | 结果 |
|---|---|
| Debug | CTest `11/11 PASS` |
| Release | CTest `11/11 PASS` |
| ASan + UBSan | CTest `11/11 PASS` |
| TSan | CTest `11/11 PASS` |
| API-only，无 QuickJS Provider | 静态库构建通过 |
| Boundary scans | S01/S02/S03/S04、Alpha stage、typed facade 全部通过 |

## 可复现命令

```sh
cmake -S . -B build-alpha-typed-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-alpha-typed-debug -j8
ctest --test-dir build-alpha-typed-debug --output-on-failure

cmake -S . -B build-alpha-typed-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-alpha-typed-release -j8
ctest --test-dir build-alpha-typed-release --output-on-failure

cmake -S . -B build-alpha-typed-asan -DCMAKE_BUILD_TYPE=Debug \
  -DQUICKAPP_JS_ENABLE_ASAN=ON -DQUICKAPP_JS_ENABLE_UBSAN=ON
cmake --build build-alpha-typed-asan -j8
ctest --test-dir build-alpha-typed-asan --output-on-failure

cmake -S . -B build-alpha-typed-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DQUICKAPP_JS_ENABLE_TSAN=ON
cmake --build build-alpha-typed-tsan -j8
ctest --test-dir build-alpha-typed-tsan --output-on-failure

cmake -S . -B build-alpha-typed-api-only -DCMAKE_BUILD_TYPE=Release \
  -DQUICKAPP_JS_BUILD_TESTS=OFF \
  -DQUICKAPP_JS_BUILD_QUICKJS_PROVIDER=OFF
cmake --build build-alpha-typed-api-only -j8
```

## 范围门禁

- 没有通用 module/method/args 或 JSON Bridge。
- 没有实现路由动作、完整能力系统、Reactive、Block、Event 或普通 RenderTransaction。
- 没有读取 RPK、文件路径或 Page IR，也没有修改公共合同和 Schema。
- `evidence/js-s04-source-manifest.sha256` 覆盖本轮源码、测试、边界脚本和本证据。

## 状态

`ALPHA_S1_TYPED_FACADE_COMPONENT READY_FOR_REVIEW`。
