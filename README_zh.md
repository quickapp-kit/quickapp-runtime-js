# QuickApp Runtime JS

[QuickApp Kit](https://github.com/quickapp-kit) 的 JS 引擎集成层。将 JavaScript 执行桥接到 C++ 运行时内核。

## 包含内容

该层提供驱动运行时的 JS 执行环境：

- **Engine API** — 抽象 JS 引擎接口（Provider 无关）
- **QuickJS Provider** — 基于 vendored QuickJS 的具体 Provider
- **Module Loader** — JS 模块解析与加载
- **Page Host** — 页面级 JS 上下文管理
- **VM Lifecycle** — JS VM 创建、销毁、内存管理
- **Binding** — Native ↔ JS 绑定层
- **Event** — JS 事件分发与处理
- **Render** — JS 驱动的渲染意图生成

## 环境要求

- C++20 / C11 编译器
- CMake 3.24+
- Vendored QuickJS 源码（通过 `QUICKAPP_JS_QUICKJS_SOURCE_DIR` 配置）

## 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### Sanitizer 构建

```bash
cmake -S . -B build-asan -G Ninja \
  -DQUICKAPP_JS_ENABLE_ASAN=ON -DQUICKAPP_JS_ENABLE_UBSAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DQUICKAPP_JS_ENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

## 目录结构

```
├── include/quickapp/js/    # 公共头文件
├── src/
│   ├── engine/             # 引擎抽象
│   ├── binding/            # Native ↔ JS 绑定
│   ├── module/             # 模块加载器
│   ├── page/               # 页面宿主控制
│   ├── vm/                 # VM 生命周期
│   ├── event/              # 事件分发
│   ├── render/             # 渲染意图
│   ├── alpha/              # Alpha 集成（初始渲染、绑定、页面阶段）
│   ├── framework/          # JS 框架门面
│   └── abi/                # ABI 层
├── providers/              # 引擎 Provider（QuickJS）
├── fakes/                  # 测试替身
├── tests/                  # 契约测试
├── cmake/                  # 构建工具
└── tools/                  # 验证脚本
```

## 相关仓库

- [quickapp-runtime-core](https://github.com/quickapp-kit/quickapp-runtime-core) — C++ 运行时内核
- [quickapp-runtime-android](https://github.com/quickapp-kit/quickapp-runtime-android) — Android 适配层
- [quickapp-runtime-lvgl](https://github.com/quickapp-kit/quickapp-runtime-lvgl) — LVGL 适配层

## 许可证

[MIT](LICENSE)
