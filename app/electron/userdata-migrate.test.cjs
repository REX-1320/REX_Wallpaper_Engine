const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { migrateUserData } = require('./userdata-migrate.cjs')

function mkAppData() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'paiint-migrate-'))
}

function seed(dir, { state = '{"version":1}' } = {}) {
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(path.join(dir, 'app-state.json'), state)
  fs.writeFileSync(path.join(dir, 'wallpaper-library.json'), '[]')
  fs.writeFileSync(path.join(dir, 'last-wallpaper.json'), '{"kind":"image","params":{"path":"x.png"}}')
  fs.mkdirSync(path.join(dir, 'thumbnails'), { recursive: true })
  fs.writeFileSync(path.join(dir, 'thumbnails', 'a.webp'), 'webp')
}

test('copies the dev profile into Paiint and leaves the source intact', () => {
  const appData = mkAppData()
  seed(path.join(appData, 'pg'))
  const res = migrateUserData(appData)

  assert.equal(res.source, path.join(appData, 'pg'))
  assert.ok(fs.existsSync(path.join(appData, 'Paiint', 'app-state.json')))
  assert.ok(fs.existsSync(path.join(appData, 'Paiint', 'thumbnails', 'a.webp')))
  assert.ok(fs.existsSync(path.join(appData, 'pg', 'app-state.json')), 'source must survive')
  assert.ok(res.copied >= 3)
  assert.deepEqual(res.errors, [])
})

test('a second run is a no-op', () => {
  const appData = mkAppData()
  seed(path.join(appData, 'pg'))
  migrateUserData(appData)

  const res = migrateUserData(appData)
  assert.equal(res.source, null)
  assert.equal(res.copied, 0)
})

test('prefers the most recently used profile when both exist', () => {
  const appData = mkAppData()
  seed(path.join(appData, 'pg'), { state: '{"version":1,"from":"dev"}' })
  const packaged = path.join(appData, 'REX')
  seed(packaged, { state: '{"version":1,"from":"packaged"}' })

  const future = Date.now() / 1000 + 60
  fs.utimesSync(path.join(packaged, 'app-state.json'), future, future)

  const res = migrateUserData(appData)
  assert.equal(res.source, packaged)
  assert.match(fs.readFileSync(path.join(appData, 'Paiint', 'app-state.json'), 'utf8'), /packaged/)
})

test('no source profile leaves no target behind', () => {
  const appData = mkAppData()
  const res = migrateUserData(appData)

  assert.equal(res.source, null)
  assert.equal(fs.existsSync(path.join(appData, 'Paiint')), false)
})
