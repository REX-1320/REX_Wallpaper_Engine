'use strict'

// ─── Minidump ───────────────────────────────────────────────────────────────
// Captures a full user-mode minidump of the runtime process via the native
// rex-minidump.exe helper (dbghelp MiniDumpWriteDump). The helper is bundled
// next to the runtime in resources/bin and falls back to the dev build tree.

const { spawn } = require('node:child_process')
const fs = require('node:fs')
const path = require('node:path')

const CAPTURE_TIMEOUT_MS = 15000

function defaultHelperPath() {
  const packaged = process.resourcesPath
    ? path.join(process.resourcesPath, 'bin', 'rex-minidump.exe')
    : null
  if (packaged && fs.existsSync(packaged)) return packaged
  const devPath = path.join(__dirname, '..', '..', '..', 'rex-core', '.build', 'Release', 'rex-minidump.exe')
  if (fs.existsSync(devPath)) return devPath
  return packaged || devPath
}

class Minidump {
  constructor({ helperPath, logLine = () => {} } = {}) {
    this.helperPath = helperPath || defaultHelperPath()
    this.logLine = logLine
  }

  // pid: process to dump. Returns { ok, path, name } or { ok: false, error }.
  capture(pid, dumpPath) {
    return new Promise((resolve) => {
      if (!pid) return resolve({ ok: false, error: 'no pid' })
      if (!fs.existsSync(this.helperPath)) {
        return resolve({ ok: false, error: `helper not found: ${this.helperPath}` })
      }
      try {
        const proc = spawn(this.helperPath, [String(pid), dumpPath], { windowsHide: true })
        let stdout = ''
        let stderr = ''
        const timer = setTimeout(() => {
          try {
            proc.kill()
          } catch {}
        }, CAPTURE_TIMEOUT_MS)

        proc.stdout.on('data', (c) => {
          stdout += c
        })
        proc.stderr.on('data', (c) => {
          stderr += c
        })
        proc.on('error', (err) => {
          clearTimeout(timer)
          resolve({ ok: false, error: err.message })
        })
        proc.on('exit', (code) => {
          clearTimeout(timer)
          if (code !== 0) {
            this.logLine('minidump: capture failed (exit', code + ')', stderr.trim().slice(0, 500))
            return resolve({ ok: false, error: stderr.trim() || `helper exit ${code}` })
          }
          let parsed = null
          try {
            parsed = JSON.parse(stdout.trim())
          } catch {}
          if (!parsed || !parsed.ok) {
            return resolve({ ok: false, error: 'helper returned no valid result' })
          }
          this.logLine('minidump: captured', parsed.path, 'pid', pid)
          resolve(parsed)
        })
      } catch (err) {
        resolve({ ok: false, error: err.message })
      }
    })
  }
}

module.exports = { Minidump }