// The legal documents the About page can show. Kept pure (no Electron
// imports) so the allowlist and both path rules are unit-testable: the
// renderer asks for a key, never a path, and an unknown key resolves to null.
const path = require('node:path')

const LEGAL_DOCS = Object.freeze({
  eula: { file: 'EULA.md', title: 'Licence agreement' },
  privacy: { file: 'PRIVACY.md', title: 'Privacy policy' },
  notices: { file: 'THIRD_PARTY_NOTICES.md', title: 'Third-party notices' },
  jsLicenses: { file: 'THIRD_PARTY_LICENSES_JS.md', title: 'JavaScript licence texts' },
})

// Packaged builds read the copy staged by electron-builder's
// `extraResources` (`resources/legal`); a dev run reads the repo's docs.
function legalDocDir({ packaged, resourcesPath, electronDir }) {
  return packaged
    ? path.join(resourcesPath, 'legal')
    : path.join(electronDir, '..', '..', 'docs', 'legal')
}

function legalDocPath(target, options) {
  const doc = Object.prototype.hasOwnProperty.call(LEGAL_DOCS, target)
    ? LEGAL_DOCS[target]
    : null
  if (!doc) return null
  return { ...doc, path: path.join(legalDocDir(options), doc.file) }
}

module.exports = { LEGAL_DOCS, legalDocDir, legalDocPath }
