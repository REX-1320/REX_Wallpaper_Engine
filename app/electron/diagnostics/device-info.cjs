'use strict'

// Local-only device identity for test-mode diagnostics. Everything here runs
// in the Electron main process and is written to the user's Downloads folder;
// nothing is uploaded. Monitor serial numbers are never read into the output.

const nodeOs = require('node:os')

const GPU_TIMEOUT_MS = 8000
const POWERSHELL_TIMEOUT_MS = 10000
const POWERSHELL_MAX_BUFFER = 4 * 1024 * 1024

// The only monitor fields that may leave this module. WmiMonitorID also
// exposes SerialNumberID; it is deliberately absent from both this list and
// the PowerShell projection below.
const MONITOR_FIELDS = [
  'instanceName',
  'manufacturer',
  'model',
  'productCode',
  'year',
  'week',
  'maxHcm',
  'maxVcm',
  'videoOutputTechnology',
]

const GPU_FIELDS = [
  'vendorId',
  'deviceId',
  'vendorString',
  'deviceString',
  'driverVersion',
  'driverDate',
  'active',
]

const HARDWARE_SCRIPT = String.raw`$ErrorActionPreference = 'SilentlyContinue'
$raw = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID
$sizes = @{}
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBasicDisplayParams | ForEach-Object {
  $sizes[$_.InstanceName] = $_
}
$connections = @{}
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorConnectionParams | ForEach-Object {
  $connections[$_.InstanceName] = $_.VideoOutputTechnology
}
$monitors = $raw | ForEach-Object {
  $size = $sizes[$_.InstanceName]
  [pscustomobject]@{
    instanceName = $_.InstanceName
    manufacturer = (([char[]]$_.ManufacturerName | Where-Object { $_ -ne 0 }) -join '')
    model = (([char[]]$_.UserFriendlyName | Where-Object { $_ -ne 0 }) -join '')
    productCode = (([char[]]$_.ProductCodeID | Where-Object { $_ -ne 0 }) -join '')
    year = $_.YearOfManufacture
    week = $_.WeekOfManufacture
    maxHcm = $size.MaxHorizontalImageSize
    maxVcm = $size.MaxVerticalImageSize
    videoOutputTechnology = $connections[$_.InstanceName]
  }
}
$gpus = Get-CimInstance Win32_VideoController | ForEach-Object {
  [pscustomobject]@{
    name = $_.Name
    driverVersion = $_.DriverVersion
    driverDate = $_.DriverDate
    adapterRam = $_.AdapterRam
    width = $_.CurrentHorizontalResolution
    height = $_.CurrentVerticalResolution
    refresh = $_.CurrentRefreshRate
  }
}
$registry = [pscustomobject]@{
  hags = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -Name HwSchMode).HwSchMode
  mpo = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\Dwm' -Name OverlayTestMode).OverlayTestMode
  gameMode = (Get-ItemProperty 'HKCU:\Software\Microsoft\GameBar' -Name AutoGameModeEnabled).AutoGameModeEnabled
}
[pscustomobject]@{ monitors = @($monitors); gpus = @($gpus); registry = $registry } |
  ConvertTo-Json -Depth 6 -Compress`

function errorMessage(err) {
  if (err && typeof err.message === 'string' && err.message) return err.message
  return String(err)
}

function optionalCall(target, method) {
  if (!target || typeof target[method] !== 'function') return null
  try {
    const value = target[method]()
    return value === undefined ? null : value
  } catch {
    return null
  }
}

// WMI stores EDID strings as arrays of UTF-16 code units padded with NULs.
function decodeWmiString(value) {
  if (typeof value === 'string') return value.replaceAll('\u0000', '').trim()
  if (!Array.isArray(value)) return ''
  let text = ''
  for (const code of value) {
    if (!Number.isFinite(code) || code === 0) continue
    text += String.fromCharCode(code)
  }
  return text.trim()
}

function monitorSizeInches(maxHcm, maxVcm) {
  if (typeof maxHcm !== 'number' || typeof maxVcm !== 'number') return null
  if (!Number.isFinite(maxHcm) || !Number.isFinite(maxVcm)) return null
  if (maxHcm <= 0 || maxVcm <= 0) return null
  return Math.round((Math.hypot(maxHcm, maxVcm) / 2.54) * 10) / 10
}

function sanitizeMonitor(monitor) {
  if (!monitor || typeof monitor !== 'object' || Array.isArray(monitor)) return null
  const clean = {}
  for (const key of MONITOR_FIELDS) {
    if (Object.prototype.hasOwnProperty.call(monitor, key)) clean[key] = monitor[key]
  }
  clean.diagonalInches = monitorSizeInches(clean.maxHcm, clean.maxVcm)
  return clean
}

function pickGpuDevices(gpuInfo) {
  if (!gpuInfo || typeof gpuInfo !== 'object') return []
  const raw = gpuInfo.gpuDevice
  const list = Array.isArray(raw) ? raw : raw && typeof raw === 'object' ? [raw] : []
  return list
    .filter((device) => device && typeof device === 'object')
    .map((device) => {
      const clean = {}
      for (const key of GPU_FIELDS) clean[key] = device[key] ?? null
      return clean
    })
}

function toArray(value) {
  if (Array.isArray(value)) return value
  if (value && typeof value === 'object') return [value]
  return []
}

function parseHardwareJson(text) {
  let parsed
  try {
    parsed = JSON.parse(text)
  } catch {
    throw new Error('invalid hardware JSON')
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    throw new Error('invalid hardware JSON')
  }

  const monitors = toArray(parsed.monitors)
    .map(sanitizeMonitor)
    .filter((monitor) => monitor !== null)
  const gpus = toArray(parsed.gpus).filter(
    (gpu) => gpu && typeof gpu === 'object' && !Array.isArray(gpu),
  )
  const registry =
    parsed.registry && typeof parsed.registry === 'object' && !Array.isArray(parsed.registry)
      ? parsed.registry
      : {}

  return { monitors, gpus, registry }
}

function withTimeout(promise, ms) {
  let timer
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`timed out after ${ms} ms`)), ms)
    if (typeof timer.unref === 'function') timer.unref()
  })
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer))
}

// Electron's GPU query can hang on a broken driver. A missing or failed GPU
// group must never reject the whole device document.
async function getGpuInfo(app) {
  const gpu = {
    devices: [],
    machineModelName: null,
    machineModelVersion: null,
    featureStatus: null,
  }

  if (app && typeof app.getGPUInfo === 'function') {
    try {
      let info = await withTimeout(
        Promise.resolve().then(() => app.getGPUInfo('complete')),
        GPU_TIMEOUT_MS,
      )
      if (typeof info === 'string') {
        try {
          info = JSON.parse(info)
        } catch {
          info = null
        }
      }
      gpu.devices = pickGpuDevices(info)
      if (info && typeof info === 'object') {
        gpu.machineModelName = info.machineModelName ?? null
        gpu.machineModelVersion = info.machineModelVersion ?? null
      }
    } catch (err) {
      gpu.error = errorMessage(err)
    }
  }

  if (app && typeof app.getGPUFeatureStatus === 'function') {
    try {
      gpu.featureStatus = (await app.getGPUFeatureStatus()) ?? null
    } catch (err) {
      if (!gpu.error) gpu.error = errorMessage(err)
    }
  }

  return gpu
}

function appInfo(app) {
  return {
    name: optionalCall(app, 'getName'),
    version: optionalCall(app, 'getVersion'),
    packaged: app && typeof app.isPackaged === 'boolean' ? app.isPackaged : null,
    locale: optionalCall(app, 'getLocale'),
    electron: process.versions.electron || null,
    chrome: process.versions.chrome || null,
    node: process.versions.node || null,
    v8: process.versions.v8 || null,
    arch: process.arch,
  }
}

function osInfo(os, platform) {
  let cpus = []
  try {
    if (typeof os.cpus === 'function') cpus = os.cpus()
  } catch {
    cpus = []
  }
  if (!Array.isArray(cpus)) cpus = []
  const cpu = cpus[0] || {}
  return {
    platform,
    release: optionalCall(os, 'release'),
    version: optionalCall(os, 'version'),
    arch: optionalCall(os, 'arch'),
    totalMemory: optionalCall(os, 'totalmem'),
    cpu: {
      model: cpu.model ?? null,
      speed: cpu.speed ?? null,
      count: cpus.length,
    },
  }
}

function pickDisplay(display) {
  const d = display && typeof display === 'object' ? display : {}
  return {
    id: d.id ?? null,
    label: d.label ?? null,
    bounds: d.bounds ?? null,
    workArea: d.workArea ?? null,
    size: d.size ?? null,
    scaleFactor: d.scaleFactor ?? null,
    rotation: d.rotation ?? null,
    internal: d.internal ?? null,
    touchSupport: d.touchSupport ?? null,
    colorDepth: d.colorDepth ?? null,
    colorSpace: d.colorSpace ?? null,
    depthPerComponent: d.depthPerComponent ?? null,
    displayFrequency: d.displayFrequency ?? null,
    nativeOrigin: d.nativeOrigin ?? null,
  }
}

function collectDisplays(screen) {
  let displays = []
  if (screen && typeof screen.getAllDisplays === 'function') {
    try {
      const list = screen.getAllDisplays()
      if (Array.isArray(list)) displays = list.map(pickDisplay)
    } catch {
      displays = []
    }
  }

  let primaryDisplayId = null
  if (screen && typeof screen.getPrimaryDisplay === 'function') {
    try {
      const primary = screen.getPrimaryDisplay()
      primaryDisplayId = primary && primary.id !== undefined ? primary.id : null
    } catch {
      primaryDisplayId = null
    }
  }

  return { displays, primaryDisplayId }
}

function collectHardware({ execFile, platform }) {
  if (platform !== 'win32') {
    return Promise.resolve({ monitors: [], gpus: [], registry: {}, error: 'not-windows' })
  }

  return new Promise((resolve) => {
    const run = typeof execFile === 'function' ? execFile : require('node:child_process').execFile
    try {
      run(
        'powershell.exe',
        ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', HARDWARE_SCRIPT],
        {
          windowsHide: true,
          timeout: POWERSHELL_TIMEOUT_MS,
          maxBuffer: POWERSHELL_MAX_BUFFER,
        },
        (err, stdout) => {
          if (err) {
            resolve({ monitors: [], gpus: [], registry: {}, error: errorMessage(err) })
            return
          }
          try {
            resolve(parseHardwareJson(String(stdout ?? '')))
          } catch (parseErr) {
            resolve({ monitors: [], gpus: [], registry: {}, error: errorMessage(parseErr) })
          }
        },
      )
    } catch (err) {
      resolve({ monitors: [], gpus: [], registry: {}, error: errorMessage(err) })
    }
  })
}

async function collectDeviceInfo(options = {}) {
  const { app, screen, execFile, platform, osModule } = options || {}
  const resolvedPlatform = platform || process.platform
  const { displays, primaryDisplayId } = collectDisplays(screen)

  const [gpu, hardware] = await Promise.all([
    getGpuInfo(app),
    collectHardware({ execFile, platform: resolvedPlatform }),
  ])

  return {
    collectedAt: new Date().toISOString(),
    app: appInfo(app),
    os: osInfo(osModule || nodeOs, resolvedPlatform),
    displays,
    primaryDisplayId,
    gpu,
    hardware,
  }
}

module.exports = {
  decodeWmiString,
  monitorSizeInches,
  sanitizeMonitor,
  pickGpuDevices,
  parseHardwareJson,
  collectDeviceInfo,
}
