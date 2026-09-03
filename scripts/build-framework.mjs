import { createHash } from 'node:crypto'
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const sourcePath = path.join(root, 'src/create-reactive-page-vm.js')
const outputRoot = path.join(root, 'dist')
const inlinePath = path.join(outputRoot, 'create-reactive-page-vm-v1.js')
const bundlePath = path.join(outputRoot, 'quickapp-framework-v1.js')
const manifestPath = path.join(outputRoot, 'quickapp-framework-v1.json')
const moduleId = '@quickapp-kit/framework-v1'

const source = (await readFile(sourcePath, 'utf8')).trim()
const inline = `${source}\n`
const indented = source.split('\n').map(line => `  ${line}`).join('\n')
const bundle = [
  `$app_define$(${JSON.stringify(moduleId)}, [], function ($app_require$, module, exports) {`,
  `  const createReactivePageVm = ${indented.trimStart()};`,
  '  module.exports = { default: { createReactivePageVm: createReactivePageVm } };',
  '});',
  '',
].join('\n')

const sha256 = value => createHash('sha256').update(value).digest('hex')
const manifest = {
  schemaVersion: 1,
  moduleId,
  moduleAbi: 'quickapp-kit-app-module-v1',
  runtimeAbi: 'quickapp-kit-runtime-v1',
  path: 'framework/quickapp-framework-v1.js',
  mime: 'application/javascript',
  byteLength: Buffer.byteLength(bundle),
  sha256: sha256(bundle),
  inline: {
    path: 'create-reactive-page-vm-v1.js',
    byteLength: Buffer.byteLength(inline),
    sha256: sha256(inline),
  },
}

await mkdir(outputRoot, { recursive: true })
await writeFile(inlinePath, inline)
await writeFile(bundlePath, bundle)
await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`)

console.log(`framework_bundle=${path.relative(root, bundlePath)}`)
console.log(`framework_sha256=${manifest.sha256}`)
