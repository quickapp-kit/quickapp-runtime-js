# Navigation Params JS Handoff

## 结论

本仓库当前是纯 JavaScript Framework，未包含 Router、Navigation Controller 或 Surface 创建实现。Navigation params 的 JS ABI 编解码和页面上下文消费合同已经存在；本任务在严格只改 JS Runtime 的约束下不需要代码修改。

## 已确认

- `NavigationPush.params` 已在共享 Runtime ABI 类型中表达为 Runtime Value 对象。
- JS Framework 不创建路由、不生成 SurfaceId，也不保存跨页面业务状态。
- 页面 VM 创建和 `onInit(context)` 调用由 Core VM Lifecycle 负责；JS Framework 保留传入的 `context.params`，不复制为全局状态。
- 缺省 `{}` 归一、params 快照写入目标 Surface、以及跨 Surface 隔离需要由 Core Router/Surface/VM Lifecycle 完成。

## JS 验证

- Command: `npm test`
- Result: Framework build passed; reactive/framework tests `3/3` passed; source boundary passed.
- Bundle: `dist/quickapp-framework-v1.js`
- SHA-256: `b53464aa786d024cd0938015caf11171ea7f3dcfa8e16f2f0f0a9d5a9e26e1af`
- Working tree had no JS source changes for this task.

## Core handoff

Core must carry `params` from `NavigationPush` into the newly created target Surface and deliver it through `SurfaceContext.params` to `onInit(context)`. Core must enforce the immutable Runtime Value snapshot and reject unsupported values before Platform Mount. No JS-side route or platform bypass is permitted.
