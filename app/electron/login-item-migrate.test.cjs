const test = require('node:test')
const assert = require('node:assert/strict')
const { cleanupLegacyLoginItem, parseRunValues, isLegacyCommand } = require('./login-item-migrate.cjs')

const SAMPLE = [
  '',
  'HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run',
  '    OneDrive    REG_SZ    "C:\\Users\\me\\OneDrive.exe" /background',
  '    REX    REG_SZ    C:\\Apps\\REX-2026-10-24.exe --background',
  '    Steam    REG_SZ    "C:\\Steam\\steam.exe" -silent',
  '',
].join('\r\n')

test('parses reg query output into name/command pairs', () => {
  const values = parseRunValues(SAMPLE)
  assert.equal(values.length, 3)
  assert.deepEqual(values[1], {
    name: 'REX',
    command: 'C:\\Apps\\REX-2026-10-24.exe --background',
  })
})

test('only legacy build paths count as legacy', () => {
  assert.equal(isLegacyCommand('C:\\Apps\\REX.exe'), true)
  assert.equal(isLegacyCommand('C:\\Apps\\rex-2026-10-24.exe --background'), true)
  assert.equal(isLegacyCommand('C:\\Apps\\Paiint.exe --background'), false)
  assert.equal(isLegacyCommand('C:\\Tools\\rex-utils\\app.exe'), false)
  assert.equal(isLegacyCommand('"C:\\Steam\\steam.exe" -silent'), false)
})

test('removes only legacy entries and reports them', () => {
  const calls = []
  const exec = (file, args) => {
    calls.push([file, ...args])
    if (args[0] === 'query') return SAMPLE
    return ''
  }

  const res = cleanupLegacyLoginItem({ platform: 'win32', exec })
  assert.deepEqual(res.removed, ['REX'])
  const deletes = calls.filter((c) => c[1] === 'delete')
  assert.equal(deletes.length, 1)
  assert.deepEqual(deletes[0].slice(3), ['/v', 'REX', '/f'])
})

test('non-Windows is a no-op', () => {
  const res = cleanupLegacyLoginItem({ platform: 'darwin', exec: () => { throw new Error('must not run') } })
  assert.deepEqual(res.removed, [])
})
