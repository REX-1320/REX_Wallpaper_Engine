// The About page reads legal documents by key, never by path. These tests pin
// the allowlist and both resolution rules, and — importantly — prove the files
// the dev path points at actually exist, so the About buttons cannot ship
// pointing at a missing document.
const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')

const { LEGAL_DOCS, legalDocDir, legalDocPath } = require('./legal-docs.cjs')

const electronDir = __dirname
const devOptions = { packaged: false, resourcesPath: 'X:\\resources', electronDir }

test('unknown keys resolve to null (the renderer cannot ask for a path)', () => {
  assert.equal(legalDocPath('eula', devOptions) !== null, true)
  for (const bad of ['', 'nope', '../EULA.md', 'C:\\Windows\\win.ini', 'eula.md', '__proto__']) {
    assert.equal(legalDocPath(bad, devOptions), null, `expected null for ${JSON.stringify(bad)}`)
  }
})

test('every allowlisted document maps to its own file and title', () => {
  const seen = new Set()
  for (const key of Object.keys(LEGAL_DOCS)) {
    const doc = legalDocPath(key, devOptions)
    assert.ok(doc, `${key} must resolve`)
    assert.equal(path.basename(doc.path), LEGAL_DOCS[key].file)
    assert.ok(doc.title.length > 0)
    assert.equal(seen.has(doc.path), false, 'two keys must not share a file')
    seen.add(doc.path)
  }
  assert.deepEqual([...seen].sort(), [
    path.join(electronDir, '..', '..', 'docs', 'legal', 'EULA.md'),
    path.join(electronDir, '..', '..', 'docs', 'legal', 'PRIVACY.md'),
    path.join(electronDir, '..', '..', 'docs', 'legal', 'THIRD_PARTY_NOTICES.md'),
    path.join(electronDir, '..', '..', 'docs', 'legal', 'THIRD_PARTY_LICENSES_JS.md'),
  ].sort())
})

test('a dev run reads the repo docs and every document exists', () => {
  assert.equal(
    legalDocDir(devOptions),
    path.resolve(electronDir, '..', '..', 'docs', 'legal'),
  )
  for (const key of Object.keys(LEGAL_DOCS)) {
    const doc = legalDocPath(key, devOptions)
    assert.equal(fs.existsSync(doc.path), true, `${doc.path} must exist`)
    assert.ok(fs.statSync(doc.path).size > 0, `${doc.path} must not be empty`)
  }
})

test('a packaged run reads resources/legal (the extraResources copy)', () => {
  const packaged = { packaged: true, resourcesPath: 'C:\\Apps\\Paiint\\resources', electronDir }
  assert.equal(legalDocDir(packaged), path.join('C:\\Apps\\Paiint\\resources', 'legal'))
  assert.equal(
    legalDocPath('privacy', packaged).path,
    path.join('C:\\Apps\\Paiint\\resources', 'legal', 'PRIVACY.md'),
  )
})

test('the package ships the legal folder as extraResources', () => {
  const pkg = JSON.parse(fs.readFileSync(path.join(electronDir, '..', 'package.json'), 'utf8'))
  const entry = pkg.build.extraResources.find((item) => item.to === 'legal')
  assert.ok(entry, 'extraResources must stage docs/legal at resources/legal')
  assert.equal(path.normalize(entry.from), path.normalize('../docs/legal'))
})
