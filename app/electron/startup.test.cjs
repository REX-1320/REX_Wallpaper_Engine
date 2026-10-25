const test = require('node:test')
const assert = require('node:assert/strict')
const { loginItemSettings, loginItemEnabled } = require('./startup.cjs')

test('a packaged app registers the executable itself', () => {
  const s = loginItemSettings(true)
  assert.equal(s.openAtLogin, true)
  assert.deepEqual(s.args, ['--background'])
  assert.equal(s.path, undefined)
})

test('dev Electron needs the app folder as argv[1]', () => {
  const s = loginItemSettings(true, 'D:\\paiint\\app')
  assert.equal(s.path, process.execPath)
  assert.deepEqual(s.args, ['D:\\paiint\\app', '--background'])
})

test('starting minimised is the default: the login item launches in the background', () => {
  assert.ok(loginItemSettings(true, 'D:\\paiint\\app').args.includes('--background'))
  assert.ok(loginItemSettings(true).args.includes('--background'))
})

test('without start-minimised the login item opens the window', () => {
  const s = loginItemSettings(true, 'D:\\paiint\\app', { startMinimized: false })
  assert.equal(s.openAtLogin, true)
  assert.deepEqual(s.args, ['D:\\paiint\\app'])
})

test('disabling the login item never carries the background flag', () => {
  const s = loginItemSettings(false, 'D:\\paiint\\app')
  assert.equal(s.openAtLogin, false)
  assert.deepEqual(s.args, ['D:\\paiint\\app', '--background'])
})

// The read-back that broke the "Launch at login" switch: in a dev build
// getLoginItemSettings() reports openAtLogin: false even though the entry was
// written, so the switch snapped back to off.
test('the login item state comes from launchItems, not from openAtLogin', () => {
  const devState = {
    launchItems: [{ name: 'electron.app.Electron', path: 'C:/electron.exe', args: ['D:/app'], scope: 'user', enabled: true }],
    executableWillLaunchAtLogin: true,
    openAtLogin: false,
  }
  assert.equal(loginItemEnabled(devState), true)

  const packagedState = { launchItems: [{ name: 'Paiint', path: 'C:/Paiint.exe', scope: 'user', enabled: true }], openAtLogin: true }
  assert.equal(loginItemEnabled(packagedState), true)
})

test('a disabled or missing login item reads as off', () => {
  assert.equal(loginItemEnabled({ launchItems: [{ enabled: false }], openAtLogin: false, executableWillLaunchAtLogin: false }), false)
  assert.equal(loginItemEnabled({ openAtLogin: false, executableWillLaunchAtLogin: false }), false)
  assert.equal(loginItemEnabled(null), false)
})
