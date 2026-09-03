import assert from 'node:assert/strict'
import { readdir, readFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const forbiddenExtensions = new Set(['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'])
const ignoredDirectories = new Set(['.git'])

async function collectFiles(directory) {
  const files = []
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    if (ignoredDirectories.has(entry.name) || entry.name.startsWith('build')) continue
    const absolute = path.join(directory, entry.name)
    if (entry.isDirectory()) files.push(...await collectFiles(absolute))
    else files.push(absolute)
  }
  return files
}

const files = await collectFiles(root)
const compiledSources = files.filter((file) => forbiddenExtensions.has(path.extname(file)))
assert.deepEqual(compiledSources, [], 'quickapp-runtime-js must not contain C/C++ source files')

const source = JSON.parse(await readFile(path.join(root, 'framework/source.json'), 'utf8'))
assert.equal(source.status, 'independent-bundle-available')
assert.equal(source.independentBundle, true)
assert.ok(source.ownedSemantics.includes('reactive-state'))
assert.ok(source.ownedSemantics.includes('render-intent'))

const emitter = await readFile(path.resolve(root, source.currentEmitter), 'utf8')
for (const marker of [
  '__qak_reactive_page_vm__',
  'new Proxy',
  'const dirty = new Set()',
  'Promise.resolve().then(flush)',
  '$quickapp_runtime_v1_submitRenderTransaction$',
]) {
  assert.ok(emitter.includes(marker), `Toolkit emitter is missing ${marker}`)
}

console.log('js_framework_source_boundary=passed')
console.log(`independent_bundle=${source.independentBundle}`)
