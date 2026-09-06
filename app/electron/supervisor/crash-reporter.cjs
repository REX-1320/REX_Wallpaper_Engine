'use strict'

// ─── CrashReporter ──────────────────────────────────────────────────────────
// Collects everything needed to diagnose a runtime failure and writes it to a
// timestamped report directory:
//
//   %TEMP%\REX\crashes\<yyyy-mm-ddThh-mm-ss.mmmZ>\
//     report.json        — what happened, when, process/exit details
//     system-info.json   — OS, CPU, memory, versions (best-effort)
//     runtime.log        — native trace log (if present)
//     crash.log          — native crash log (if present)
//     workerw_diag.log   — desktop-layer diagnostics (if present)
//     rex-app.log        — Electron-side log (if present)
//     <name>.dmp         — minidump captured by rex-minidump.exe (optional)

const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')

const MAX_COPIED_LOG_BYTES = 4 * 1024 * 1024

function systemInfo() {
  return {
    platform: process.platform,
    arch: process.arch,
    osType: os.type(),
    osRelease: os.release(),
    osVersion: os.version(),
    cpus: os.cpus().map((c) => ({ model: c.model.trim(), speedMHz: c.speed })),
    cores: os.cpus().length,
    totalMemoryMB: Math.round(os.totalmem() / (1024 * 1024)),
    freeMemoryMB: Math.round(os.freemem() / (1024 * 1024)),
    hostname: os.hostname(),
    userHome: os.homedir(),
    uptimeSec: Math.round(os.uptime()),
    versions: {
      node: process.versions.node,
      chrome: process.versions.chrome,
      electron: process.versions.electron,
    },
    pid: process.pid,
    startedAt: new Date(Date.now() - process.uptime() * 1000).toISOString(),
  }
}

class CrashReporter {
  constructor({ baseDir, logLine = () => {} } = {}) {
    this.baseDir = baseDir || path.join(os.tmpdir(), 'REX', 'crashes')
    this.logLine = logLine
  }

  _copyLogIfPresent(src, destDir, destName) {
    if (!src) return
    try {
      if (!fs.existsSync(src)) return
      const stat = fs.statSync(src)
      if (!stat.isFile() || stat.size === 0) return
      if (stat.size > MAX_COPIED_LOG_BYTES) {
        // Tail-copy: diagnostics live at the end of rotating logs.
        const fd = fs.openSync(src, 'r')
        const buf = Buffer.alloc(MAX_COPIED_LOG_BYTES)
        fs.readSync(fd, buf, 0, buf.length, Math.max(0, stat.size - buf.length))
        fs.closeSync(fd)
        fs.writeFileSync(path.join(destDir, destName), buf)
        return
      }
      fs.copyFileSync(src, path.join(destDir, destName))
    } catch {
      // log collection is best-effort
    }
  }

  // report: { reason, exitCode?, pid?, minidumpPath?, state?, extra? }
  writeReport(report) {
    let dir = null
    try {
      const stamp = new Date().toISOString().replace(/[:.]/g, '-')
      dir = path.join(this.baseDir, stamp)
      fs.mkdirSync(dir, { recursive: true })

      const reportJson = {
        app: { name: 'REX', mainPid: process.pid },
        timestamp: new Date().toISOString(),
        reason: report.reason,
        exitCode: report.exitCode ?? null,
        processId: report.pid ?? null,
        minidump: report.minidumpPath ?? null,
        recoveryState: report.state ?? null,
        extra: report.extra ?? {},
      }
      fs.writeFileSync(path.join(dir, 'report.json'), JSON.stringify(reportJson, null, 2))
      fs.writeFileSync(path.join(dir, 'system-info.json'), JSON.stringify(systemInfo(), null, 2))

      const logs = report.logs ?? {}
      this._copyLogIfPresent(logs.runtimeLog, dir, 'runtime.log')
      this._copyLogIfPresent(logs.crashLog, dir, 'crash.log')
      this._copyLogIfPresent(logs.workerwDiagLog, dir, 'workerw_diag.log')
      this._copyLogIfPresent(logs.appLog, dir, 'rex-app.log')

      this.logLine('crash-reporter: report written to', dir)
      return dir
    } catch (err) {
      this.logLine('crash-reporter: writeReport ERROR', err.message)
      return null
    }
  }
}

module.exports = { CrashReporter }