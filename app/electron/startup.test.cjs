const test = require('node:test')
const assert = require('node:assert/strict')
const { loginItemSettings } = require('./startup.cjs')

test('a packaged app registers the executable itself', () => {
  const s = loginItemSettings(true)
  assert.equal(s.openAtLogin, true)
  assert.deepEqual(s.args, ['--background'])
  assert.equal(s.path, undefined)
})

test('dev Electron needs the app folder as argv[1]', () => {
  const s = loginItemSettings(true, 'D:\\REX_doc\\app')
  assert.equal(s.path, process.execPath)
  assert.deepEqual(s.args, ['D:\\REX_doc\\app', '--background'])
})

test('starting minimised is the default: the login item launches in the background', () => {
  assert.ok(loginItemSettings(true, 'D:\\REX_doc\\app').args.includes('--background'))
  assert.ok(loginItemSettings(true).args.includes('--background'))
})

test('without start-minimised the login item opens the window', () => {
  const s = loginItemSettings(true, 'D:\\REX_doc\\app', { startMinimized: false })
  assert.equal(s.openAtLogin, true)
  assert.deepEqual(s.args, ['D:\\REX_doc\\app'])
})

test('disabling the login item never carries the background flag', () => {
  const s = loginItemSettings(false, 'D:\\REX_doc\\app')
  assert.equal(s.openAtLogin, false)
  assert.deepEqual(s.args, ['D:\\REX_doc\\app', '--background'])
})
