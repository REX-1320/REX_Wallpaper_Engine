const { execSync } = require('node:child_process')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')

const projectDir = path.join(__dirname, '..')
const out = path.join(os.tmpdir(), 'rex-electron-release')

fs.rmSync(out, { recursive: true, force: true })
fs.mkdirSync(out, { recursive: true })

execSync(
  `npx electron-builder --win portable --config.directories.output="${out}"`,
  { stdio: 'inherit', cwd: projectDir },
)

const dest = path.join(projectDir, 'dist-exe')
fs.mkdirSync(dest, { recursive: true })
fs.copyFileSync(path.join(out, 'REX.exe'), path.join(dest, 'REX.exe'))
fs.rmSync(out, { recursive: true, force: true })

console.log('Portable exe:', path.join(dest, 'REX.exe'))
