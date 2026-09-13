// ============================================================================
// 第一性：这是 JS↔Core 边界上、JS 侧唯一的活件——一台通用的「响应式翻译机」。
// 职责只有一个：把「应用改了状态」翻译成「给 Core 的一批 ID+值 的事务」。
// 它本身不含任何应用信息（通用、恒等），应用信息全靠入参喂进来（料，随页而变）：
//   target           = 页面状态对象（含数据字段与方法），被 Proxy 劫持
//   context          = 运行时上下文，关键是 surfaceId（这一屏的身份）
//   bindings         = 绑定表 {templateBindingId: {deps, evaluate}}，编译期产出
//   blockDefinitions = 动态节点表(if/for) {templateBlockId: {...}}，编译期产出
// 它输出的东西里只有 ID 和值，没有树——树的 owner 是 Core，这边不持有树。
// ============================================================================
(function createReactivePageVm(target, context, bindings, blockDefinitions = {}) {
  // —— 机器的内部状态，owner 全在 JS 侧，Core 碰不到 ——
  let scheduled = false          // microtask 是否已排队，保证一轮同步写只 flush 一次
  let revision = 0               // 已被 Core 确认的版本号；失败不推进，两侧据此保持一致
  let sequence = 0               // 事务自增序号，仅用于生成事务 ID
  const dirty = new Set()        // 脏集合：本轮被改动波及的 bindingId（与 block 标记）
  let activeBlocks = new Map()   // 当前生效的 block 实例快照（id -> 实例信息）
  let activeBlockSlots = new Map()   // slot(定位) -> 实例 id，用于跨轮复用同一实例
  const blockGenerations = new Map() // slot -> 代数，实例销毁重建时用来生成不冲突的新 id
  let proxy

  // slot = 一个 block 实例的稳定「位置身份」= 模板块 id + 迭代 key。
  // 同一 slot 跨轮指向同一实例，是「局部更新而非重建」的地基。
  const blockSlot = function (definition, key) {
    return String(definition.templateBlockId) + "\u0000" + String(key)
  }
  // 实例 id 是给 Core 的稳定寻址 key（NodeID 体系在 block 侧的对应物）。
  // 同 slot 复用则 id 不变；重建则靠 generation 递增，避免与已删实例撞 id。
  const blockInstanceId = function (definition, key, generation) {
    const base = "blk:" + context.surfaceId + "-" +
      String(definition.templateBlockId) + "-" +
      String(key).replace(/[^A-Za-z0-9_.-]/g, "_")
    return generation === 1 ? base : base + "-g" + String(generation)
  }
  // collectBlocks：跑一遍 if/for 的 evaluate，算出「此刻应该存在哪些 block 实例、各在什么位置」。
  // 本质是「当前状态 -> 期望的动态节点列表」的纯计算，不产生任何副作用/事务。
  const collectBlocks = function () {
    const result = []
    // dynamicOffsets：同一父节点下，动态实例会挤占静态兄弟的位置，这里累加位移，
    // 让每个实例的最终 index 落在 Core 树里正确的插入点上。
    const dynamicOffsets = {}
    Object.keys(blockDefinitions)
      .sort(function (left, right) { return Number(left) - Number(right) })
      .forEach(function (id) {
        const definition = blockDefinitions[id]
        const parentKey = String(definition.parentTemplateNodeId)
        const append = function (key, scope) {
          const offset = dynamicOffsets[parentKey] || 0
          result.push({
            definition: definition,
            key: key,
            scope: scope,
            index: definition.staticIndex + offset,
          })
          dynamicOffsets[parentKey] = offset + 1
        }
        if (definition.kind === "if") {
          if (definition.evaluate.call(proxy, {})) append("if", {})
          return
        }
        const items = definition.evaluate.call(proxy, {})
        if (!Array.isArray(items)) return
        items.forEach(function (item, index) {
          const scope = {}
          scope[definition.indexAlias] = index
          scope[definition.itemAlias] = item
          append(definition.key.call(proxy, scope), scope)
        })
      })
    return result
  }
  // 求一个 block 实例内部各绑定点的当前值。scope 带着 for 的 item/index，
  // 所以同一模板在不同迭代项下能算出不同的值。
  const blockBindings = function (item) {
    const values = {}
    Object.keys(item.definition.bindings).forEach(function (id) {
      values[id] = item.definition.bindings[id].call(proxy, item.scope)
    })
    return values
  }
  // reconcileBlocks：机器的「diff 核心」，但 diff 的是 block 实例集合，不是节点树。
  // 拿 collectBlocks 的「期望集合」与 activeBlocks 的「现有集合」对比，产出最小操作：
  //   新出现 -> instantiateBlock，位置变了 -> moveBlock，消失了 -> removeBlock。
  // initial=true 时是首帧，只会产生 instantiate（无旧集合可比，也不发 move/remove）。
  const reconcileBlocks = function (initial) {
    const nextBlocks = new Map()
    const operations = []
    collectBlocks().forEach(function (item) {
      const slot = blockSlot(item.definition, item.key)
      const previousId = activeBlockSlots.get(slot)
      const previous = previousId === undefined
        ? undefined
        : activeBlocks.get(previousId)
      const generation = blockGenerations.get(slot) || 0
      const id = previousId === undefined
        ? blockInstanceId(item.definition, item.key, generation + 1)
        : previousId
      if (previousId === undefined) blockGenerations.set(slot, generation + 1)
      if (previous === undefined) {
        const handlers = item.definition.handlers.map(function (handler) {
          return {
            ownerInstanceId: id,
            templateHandlerId: handler.templateHandlerId,
            handlerId: "hdl:" + context.surfaceId + "-" +
              String(handler.templateHandlerId) + "-" + id,
          }
        })
        operations.push({
          kind: "instantiateBlock",
          templateBlockId: item.definition.templateBlockId,
          blockInstanceId: id,
          parent: {
            ownerInstanceId: "cmp:" + context.surfaceId,
            templateNodeId: item.definition.parentTemplateNodeId,
          },
          index: item.index,
          key: item.key,
          initialBindings: blockBindings(item),
          handlers: handlers,
        })
      } else if (!initial && previous.index !== item.index) {
        operations.push({
          kind: "moveBlock",
          blockInstanceId: id,
          parent: {
            ownerInstanceId: "cmp:" + context.surfaceId,
            templateNodeId: item.definition.parentTemplateNodeId,
          },
          index: item.index,
        })
      }
      nextBlocks.set(id, {
        definition: item.definition,
        key: item.key,
        scope: item.scope,
        index: item.index,
        slot: slot,
      })
    })
    if (!initial) {
      activeBlocks.forEach(function (_previous, id) {
        if (!nextBlocks.has(id)) {
          operations.push({ kind: "removeBlock", blockInstanceId: id })
        }
      })
    }
    const nextSlots = new Map()
    nextBlocks.forEach(function (value, id) {
      nextSlots.set(value.slot, id)
    })
    return {
      operations: operations,
      nextBlocks: nextBlocks,
      nextSlots: nextSlots,
    }
  }
  const commitBlocks = function (nextBlocks, nextSlots) {
    activeBlocks = nextBlocks
    activeBlockSlots = nextSlots
  }
  const prepareInitialBlocks = function () {
    scheduled = false
    dirty.clear()
    activeBlocks = new Map()
    activeBlockSlots = new Map()
    blockGenerations.clear()
    const initialBlocks = reconcileBlocks(true)
    commitBlocks(initialBlocks.nextBlocks, initialBlocks.nextSlots)
    return initialBlocks.operations
  }
  // ==========================================================================
  // Task 2：翻译并提交（把「脏」变成「一批 ID+值 的事务」发给 Core）
  // 在 microtask 里跑一次。做两件合流的事：
  //   (a) 遍历脏 bindingId，跑 evaluate 得新值 -> updateBinding
  //   (b) reconcileBlocks(false) 算 if/for 的增删移 -> instantiate/move/removeBlock
  // 打包成一个事务 submitRenderTransaction 给 Core；Core 回 ok 才推进 revision、
  // 清脏、提交 block 快照——失败不推进，保证 JS 与 Core 两侧状态一致。
  // ==========================================================================
  const flush = function () {
    scheduled = false
    if (dirty.size === 0) return
    const operations = []
    dirty.forEach(function (id) {
      const binding = bindings[id]
      if (binding === undefined) return
      operations.push({
        kind: "updateBinding",
        ownerInstanceId: "cmp:" + context.surfaceId,
        templateBindingId: Number(id),
        value: binding.evaluate.call(proxy, {}),
      })
    })
    const blocks = reconcileBlocks(false)
    blocks.operations.forEach(function (operation) {
      operations.push(operation)
    })
    if (operations.length === 0) return
    const nextRevision = revision + 1
    const causalRequest = globalThis.$quickapp_current_request_id$
    const message = {
      schemaVersion: 1,
      surfaceId: context.surfaceId,
      transactionId: "txn:" + context.surfaceId + "-" + String(++sequence),
      revision: nextRevision,
      operations: operations,
    }
    if (typeof causalRequest === "string" && causalRequest.indexOf("req:") === 0) {
      message.requestId = causalRequest
    }
    const result = globalThis.$quickapp_runtime_v1_submitRenderTransaction$(message)
    if (result && result.ok === true) {
      revision = nextRevision
      dirty.clear()
      commitBlocks(blocks.nextBlocks, blocks.nextSlots)
    }
  }

  // ==========================================================================
  // Task 1：劫持状态，收集脏（响应式的入口）
  // Proxy 拦截对 state 的写。每次写：靠编译期算好的 deps 反查，标脏受影响的
  // bindingId / block，然后挂一个 microtask（一轮同步写只挂一次）。
  // 本 Task 只「标脏 + 排期」，不做任何求值/提交——那是 Task 2 的事。
  // ==========================================================================
  proxy = new Proxy(target, {
    set: function (object, property, value) {
      object[property] = value
      const name = String(property)
      Object.keys(bindings).forEach(function (id) {
        if (bindings[id].deps.indexOf(name) >= 0) dirty.add(id)
      })
      Object.keys(blockDefinitions).forEach(function (id) {
        if (blockDefinitions[id].deps.indexOf(name) >= 0) {
          dirty.add("__qak_block__")
        }
      })
      if (!scheduled) {
        scheduled = true
        Promise.resolve().then(flush)
      }
      return true
    },
  })
  // ==========================================================================
  // Task 3：交出首帧入口（供 Core 拉取初始动态节点）
  // 暴露 __qak_initial_blocks__ getter：Core 读它时触发 prepareInitialBlocks，
  // 全量算一遍初始 if/for 实例（reconcileBlocks(true)，只产 instantiate）并作为
  // 首帧的动态部分交给 Core。静态节点走 Page IR，这里只补动态那部分。
  // 此后的变化都由 Task 1→Task 2 增量驱动。
  // ==========================================================================
  Object.defineProperty(proxy, "__qak_initial_blocks__", {
    get: prepareInitialBlocks,
  })
  return proxy
})
