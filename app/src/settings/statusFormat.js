// Turns the runtime's GET_STATUS telemetry into display rows.
//
// Pure: no React, no IPC, no Electron. The main process hands the raw
// payload over and the renderer decides what to show, so the formatting is
// testable on its own (`node --test app/src/settings/statusFormat.test.js`).

const MIB = 1024 * 1024

function mib(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return null
  return `${Math.round(bytes / MIB)} MiB`
}

// The runtime reports the process' private memory in MEGABYTES
// (GetProcessPrivateMemoryMB), while the GPU counters are raw bytes.
function megabytes(value) {
  if (!Number.isFinite(value) || value <= 0) return null
  return `${Math.round(value)} MiB`
}

function rate(value, suffix) {
  if (!Number.isFinite(value) || value <= 0) return null
  return `${value.toFixed(1)} ${suffix}`
}

function ms(value) {
  if (!Number.isFinite(value)) return null
  return `${value.toFixed(1)} ms`
}

/** The file's name, not its whole path: a panel row is a summary, and a
 *  120-character path breaks the row. The full path stays available in the
 *  row's tooltip. */
function fileName(value) {
  if (typeof value !== 'string' || !value) return null
  const parts = value.split(/[\\/]/)
  return parts[parts.length - 1] || value
}

// The engine's state enum (IWallpaperEngine.hpp:23).
// The raw number is what GET_STATUS reports; the names are what a user reads.
const ENGINE_STATES = {
  0: 'uninitialized',
  1: 'initializing',
  2: 'idle',
  3: 'running',
  4: 'paused',
  5: 'stopped',
  6: 'error',
  7: 'shutting down',
}

/**
 * @param {object|null} payload the raw GET_STATUS response
 * @returns {{ state: string, rows: Array<{ label: string, value: string }> }}
 */
export function formatStatus(payload) {
  if (!payload || typeof payload !== 'object') return { state: 'unknown', rows: [] }

  const state = ENGINE_STATES[payload.state] ?? 'unknown'

  const tel = payload.telemetry ?? {}
  const scene = payload.scene ?? {}
  const rows = []

  const push = (label, value) => {
    if (value == null || value === '') return
    rows.push({ label, value: String(value) })
  }

  push('Frame rate', rate(tel.renderFps, 'fps'))
  push('Source rate', rate(tel.sourceFps, 'fps'))
  push('Display', rate(tel.displayHz, 'Hz'))
  push('Dropped frames', Number.isFinite(tel.schedulerDrops) ? String(tel.schedulerDrops) : null)
  push('A/V drift', ms(tel.avSyncDriftMs))
  push('VRAM (dedicated)', mib(tel.gpuDedicated))
  push('VRAM (shared)', mib(tel.gpuShared))
  push('VRAM budget', mib(tel.gpuBudget))
  push('Process memory', megabytes(payload.privateBytes))
  if (tel.hdrActive === true) push('HDR', 'Active')
  else if (tel.hdrActive === false) push('HDR', 'Off')
  push('Display peak', Number.isFinite(tel.displayMaxNits) && tel.displayMaxNits > 0 ? `${tel.displayMaxNits} nits` : null)
  push('File', fileName(scene.path))

  return { state, rows }
}
