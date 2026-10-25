const { execSync } = require('node:child_process')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')

const projectDir = path.join(__dirname, '..')
const pkg = JSON.parse(fs.readFileSync(path.join(projectDir, 'package.json'), 'utf8'))
const out = path.join(os.tmpdir(), 'paiint-electron-release')

fs.rmSync(out, { recursive: true, force: true })
fs.mkdirSync(out, { recursive: true })

execSync(
  `npx electron-builder --win portable --config.directories.output="${out}"`,
  { stdio: 'inherit', cwd: projectDir },
)

const dest = path.join(projectDir, 'dist-exe')
fs.mkdirSync(dest, { recursive: true })

// Version the artefact by the package version plus a LOCAL date (a UTC stamp
// would name an evening build with yesterday's date). A tester holding two
// builds of the same version can still tell them apart; a repeat build on the
// same day gets `-b`, `-c`, ... instead of silently overwriting the previous exe.
const now = new Date()
const ymd = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')}`
const baseName = `Paiint-${pkg.version}-${ymd}`
let exeName = `${baseName}.exe`
// Matches the established tester naming: first build of the day has no
// suffix, the next is `-b`, then `-c`, ...
for (let i = 2; fs.existsSync(path.join(dest, exeName)); i += 1) {
  exeName = `${baseName}-${String.fromCharCode(96 + i)}.exe`
}
fs.copyFileSync(path.join(out, 'Paiint.exe'), path.join(dest, exeName))

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

console.log('Portable exe:', path.join(dest, exeName))
