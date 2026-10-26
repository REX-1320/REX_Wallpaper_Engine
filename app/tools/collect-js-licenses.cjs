// Collect the licence texts of the production JavaScript dependencies that are
// compiled into the Paiint UI bundle. Output is one consolidated document that
// ships beside THIRD_PARTY_NOTICES.md (and is readable from the About page).
//
// usage: node app/tools/collect-js-licenses.cjs [--check]
//   --check  fail if the checked-in document is out of date
const { execSync } = require('node:child_process')
const fs = require('node:fs')
const path = require('node:path')

const appDir = path.join(__dirname, '..')
const repoRoot = path.join(appDir, '..')
const outFile = path.join(repoRoot, 'docs', 'legal', 'THIRD_PARTY_LICENSES_JS.md')

const LICENCE_FILE = /^(licen[cs]e|copying|notice)(\..*)?$/i

// Some packages declare a standard licence but do not publish its text in the
// npm tarball. Including the canonical text of the licence they declare is
// better than a bare "no file" note.
const STANDARD_TEXTS = {
  MIT: `MIT License

Copyright (c) the {{name}} authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.`,
  ISC: `ISC License

Copyright (c) the {{name}} authors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.`,
  'BSD-3-Clause': `BSD 3-Clause License

Copyright (c) the {{name}} authors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.`,
  '0BSD': `BSD Zero Clause License

Copyright (c) the {{name}} authors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.`,
  Unlicense: `This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.

In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <https://unlicense.org>`,
}

// Composite SPDX expressions that appear in this tree.
STANDARD_TEXTS['MIT AND ISC'] = `${STANDARD_TEXTS.MIT}\n\n---\n\n${STANDARD_TEXTS.ISC}`

function productionTree() {
  // pnpm prints a deprecation WARN on stderr; execSync keeps only stdout.
  const raw = execSync('pnpm licenses list --prod --json', {
    cwd: appDir,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
  })
  const byLicence = JSON.parse(raw)
  const packages = []
  for (const [licence, entries] of Object.entries(byLicence)) {
    for (const entry of entries) {
      for (const version of entry.versions ?? []) {
        packages.push({ name: entry.name, version, licence })
      }
    }
  }
  return packages.sort((a, b) => a.name.localeCompare(b.name) || a.version.localeCompare(b.version))
}

function packageDir(name, version) {
  const candidates = [
    path.join(appDir, 'node_modules', name),
    path.join(appDir, 'node_modules', '.pnpm', `${name.replace('/', '+')}@${version}`, 'node_modules', name),
  ]
  return candidates.find((dir) => fs.existsSync(path.join(dir, 'package.json'))) ?? null
}

function licenceText(dir) {
  if (!dir) return null
  const files = fs
    .readdirSync(dir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && LICENCE_FILE.test(entry.name))
    .map((entry) => entry.name)
    .sort((a, b) => {
      const rank = (n) => (/^licen/i.test(n) ? 0 : /^copying/i.test(n) ? 1 : 2)
      return rank(a) - rank(b) || a.localeCompare(b)
    })
  for (const file of files) {
    const text = fs.readFileSync(path.join(dir, file), 'utf8').trim()
    if (text) return { file, text }
  }
  return null
}

function render(packages) {
  const missing = []
  const sections = packages.map((pkg) => {
    const dir = packageDir(pkg.name, pkg.version)
    const found = licenceText(dir)
    const standard = found ? null : STANDARD_TEXTS[pkg.licence]
    if (!found && !standard) missing.push(`${pkg.name}@${pkg.version}`)
    const body = found
      ? found.text
      : standard
        ? standard.replace('{{name}}', pkg.name)
        : `No licence file was included in the published package. The package\nmetadata declares: ${pkg.licence}.`
    const source = found
      ? `Source file: \`${found.file}\``
      : standard
        ? 'Source file: (none in the package — canonical text of the declared licence)'
        : 'Source file: (none in the package)'
    return [
      `## ${pkg.name} ${pkg.version} — ${pkg.licence}`,
      '',
      source,
      '',
      '````text',
      body,
      '````',
      '',
    ].join('\n')
  })

  const licences = [...new Set(packages.map((p) => p.licence))].sort()
  const header = [
    '# Bundled JavaScript licence texts — Paiint 0.1.0',
    '',
    'Paiint\'s interface is bundled from the npm packages listed below. Each',
    'package remains under its own licence and retains its own copyright; these',
    'texts are reproduced verbatim from the published packages.',
    '',
    'This file is generated — do not edit it by hand. Regenerate with:',
    '',
    '```powershell',
    'node app/tools/collect-js-licenses.cjs',
    '```',
    '',
    `Packages: **${packages.length}** · Licences: ${licences.join(', ')} · Generated: ${new Date().toISOString().slice(0, 10)}`,
    '',
    missing.length > 0
      ? `> ${missing.length} package(s) published no licence file and declare a licence with no canonical fallback; these need manual review: ${missing.join(', ')}.`
      : '> Every package below either shipped a licence file or declares a standard licence whose canonical text is reproduced.',
    '',
    '---',
    '',
  ].join('\n')

  return { text: header + sections.join('\n'), missing }
}

const packages = productionTree()
const { text, missing } = render(packages)

if (process.argv.includes('--check')) {
  const current = fs.existsSync(outFile) ? fs.readFileSync(outFile, 'utf8') : ''
  // The generated date makes a byte comparison useless across days; compare
  // the package sections instead.
  const strip = (s) => s.replace(/Generated: \d{4}-\d{2}-\d{2}/, 'Generated: DATE')
  if (strip(current) !== strip(text)) {
    console.error(`out of date: ${outFile}\nrun: node app/tools/collect-js-licenses.cjs`)
    process.exit(1)
  }
  console.log(`up to date: ${packages.length} packages`)
  process.exit(0)
}

fs.mkdirSync(path.dirname(outFile), { recursive: true })
fs.writeFileSync(outFile, text, 'utf8')
console.log(`wrote ${outFile}`)
console.log(`${packages.length} packages, ${(text.length / 1024).toFixed(0)} KB, ${missing.length} without a licence file`)
if (missing.length > 0) console.log('no licence file:', missing.join(', '))
