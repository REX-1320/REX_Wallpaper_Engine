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

// The unpacked folder is the OTHER thing that gets copied to another
// machine for testing, and it must be in sync with the portable exe: a
// tester who takes the folder would otherwise run the OLD native runtime
// (from a previous build) with the new UI and conclude the engine changes
// are broken. electron-builder writes win-unpacked next to the portable
// exe, so copy it out before the cleanup below removes the temp dir.
const unpacked = path.join(out, 'win-unpacked')
if (fs.existsSync(unpacked)) {
  const destUnpacked = path.join(dest, 'win-unpacked')
  fs.rmSync(destUnpacked, { recursive: true, force: true })
  fs.cpSync(unpacked, destUnpacked, { recursive: true })
  console.log('Unpacked folder:', destUnpacked)
}

fs.rmSync(out, { recursive: true, force: true })

console.log('Portable exe:', path.join(dest, 'REX.exe'))
