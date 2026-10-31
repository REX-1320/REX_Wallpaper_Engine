'use strict'

const test = require('node:test')
const assert = require('node:assert/strict')

const { parseOcclusionLine } = require('./occlusion.cjs')

test('CLEAR means the desktop is visible', () => {
  assert.deepEqual(parseOcclusionLine('CLEAR'), { occluded: false, detail: '' })
  assert.deepEqual(parseOcclusionLine('  CLEAR  '), { occluded: false, detail: '' })
})

test('OCCLUDED carries the covering window for the log', () => {
  assert.deepEqual(parseOcclusionLine('OCCLUDED Notepad 800x600'), {
    occluded: true,
    detail: 'Notepad 800x600',
  })
  assert.deepEqual(parseOcclusionLine('OCCLUDED'), { occluded: true, detail: '' })
})

test('anything that is not a state line is ignored', () => {
  assert.equal(parseOcclusionLine(''), null)
  assert.equal(parseOcclusionLine('Add-Type: compiled'), null)
  assert.equal(parseOcclusionLine('OCCLUDEDX'), null)
  assert.equal(parseOcclusionLine(null), null)
  assert.equal(parseOcclusionLine(42), null)
})
