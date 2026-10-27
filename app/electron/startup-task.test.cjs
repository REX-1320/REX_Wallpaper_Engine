const test = require('node:test')
const assert = require('node:assert/strict')
const { parseState, isEnabled } = require('./startup-task.cjs')

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

test('isEnabled: only enabled states count', () => {
  assert.equal(isEnabled('enabled'), true)
  assert.equal(isEnabled('enabled-by-policy'), true)
  assert.equal(isEnabled('disabled'), false)
  assert.equal(isEnabled('disabled-by-user'), false)
  assert.equal(isEnabled('error'), false)
  assert.equal(isEnabled(null), false)
})
