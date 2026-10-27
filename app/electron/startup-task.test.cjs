const test = require('node:test')
const assert = require('node:assert/strict')
const path = require('node:path')
const { parseState, query, isEnabled, UNAVAILABLE } = require('./startup-task.cjs')

test('parseState: helper states', () => {
  assert.equal(parseState('enabled\n'), 'enabled')
  assert.equal(parseState('disabled\n'), 'disabled')
  assert.equal(parseState('disabled-by-user\n'), 'disabled-by-user')
  assert.equal(parseState('disabled-by-policy\n'), 'disabled-by-policy')
  assert.equal(parseState('enabled-by-policy\n'), 'enabled-by-policy')
})

test('parseState: unpackaged returns null so the caller falls back', () => {
  assert.equal(parseState('unpackaged\n'), null)
  assert.equal(parseState(''), null)
  assert.equal(parseState(undefined), null)
})

test('parseState: unknown output is an error, never a fake success', () => {
  assert.equal(parseState('error\n'), 'error')
  assert.equal(parseState('something else'), 'error')
})

test('query: missing stub is portable, a mute helper is unavailable', () => {
  // No stub on disk: the portable/dev build must use the Run key.
  assert.equal(query(path.join(__dirname, 'no-such-stub.exe')), null)
  // A real executable that does not understand --status: it exists but never
  // prints a state, which must not be mistaken for "unpackaged".
  assert.equal(query(process.execPath), UNAVAILABLE)
})

test('isEnabled: only enabled states count', () => {
  assert.equal(isEnabled('enabled'), true)
  assert.equal(isEnabled('enabled-by-policy'), true)
  assert.equal(isEnabled('disabled'), false)
  assert.equal(isEnabled('disabled-by-user'), false)
  assert.equal(isEnabled('error'), false)
  assert.equal(isEnabled(null), false)
})
