import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import path from 'node:path'
import test from 'node:test'
import vm from 'node:vm'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')

async function loadFactory(transactions) {
  const source = await readFile(path.join(root, 'src/create-reactive-page-vm.js'), 'utf8')
  return vm.runInNewContext(source, {
    $quickapp_runtime_v1_submitRenderTransaction$(message) {
      transactions.push(JSON.parse(JSON.stringify(message)))
      return { ok: true }
    },
  })
}

test('reactive state batches bindings into one RenderTransaction', async () => {
  const transactions = []
  const createReactivePageVm = await loadFactory(transactions)
  {
    const page = createReactivePageVm(
      { count: 0, label: 'zero' },
      { surfaceId: 'srf:1' },
      {
        1: { deps: ['count'], evaluate() { return this.count } },
        2: { deps: ['label'], evaluate() { return this.label } },
      },
    )
    page.count = 1
    page.label = 'one'
    await Promise.resolve()
    assert.equal(transactions.length, 1)
    assert.deepEqual(
      transactions[0].operations.map(operation => [operation.kind, operation.templateBindingId, operation.value]),
      [['updateBinding', 1, 1], ['updateBinding', 2, 'one']],
    )
  }
})

test('if and keyed for blocks preserve identity and emit structural operations', async () => {
  const transactions = []
  const createReactivePageVm = await loadFactory(transactions)
  {
    const page = createReactivePageVm(
      { visible: false, items: [{ id: 'a', title: 'A' }] },
      { surfaceId: 'srf:2' },
      {},
      {
        1: {
          templateBlockId: 1,
          kind: 'if',
          parentTemplateNodeId: 1,
          staticIndex: 0,
          deps: ['visible'],
          bindings: {},
          handlers: [],
          evaluate() { return this.visible },
        },
        2: {
          templateBlockId: 2,
          kind: 'for',
          parentTemplateNodeId: 1,
          staticIndex: 1,
          deps: ['items'],
          indexAlias: 'index',
          itemAlias: 'item',
          bindings: { 3(scope) { return scope.item.title } },
          handlers: [{ templateHandlerId: 7 }],
          evaluate() { return this.items },
          key(scope) { return scope.item.id },
        },
      },
    )
    assert.equal(page.__qak_initial_blocks__.length, 1)
    const originalId = page.__qak_initial_blocks__[0].blockInstanceId

    page.visible = true
    page.items = [{ id: 'a', title: 'A' }, { id: 'b', title: 'B' }]
    await Promise.resolve()
    assert.equal(transactions.length, 1)
    const instantiate = transactions[0].operations.filter(operation => operation.kind === 'instantiateBlock')
    assert.equal(instantiate.length, 2)
    assert.equal(instantiate.some(operation => operation.blockInstanceId === originalId), false)

    page.visible = false
    page.items = [{ id: 'b', title: 'B' }]
    await Promise.resolve()
    assert.equal(transactions.length, 2)
    assert.equal(transactions[1].operations.some(operation => operation.kind === 'removeBlock'), true)
  }
})

test('built shared bundle registers the frozen default export shape', async () => {
  const bundle = await readFile(path.join(root, 'dist/quickapp-framework-v1.js'), 'utf8')
  const registry = new Map()
  vm.runInNewContext(bundle, {
    $app_define$(moduleId, dependencies, factory) {
      const module = { exports: {} }
      factory(() => undefined, module, module.exports)
      registry.set(moduleId, { dependencies, exports: module.exports })
    },
  })
  const registered = registry.get('@quickapp-kit/framework-v1')
  assert.ok(registered)
  assert.equal(registered.dependencies.length, 0)
  assert.equal(typeof registered.exports.default.createReactivePageVm, 'function')
})
