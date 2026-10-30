'use strict'

const test = require('node:test')
const assert = require('node:assert/strict')

const {
  decodeWmiString,
  monitorSizeInches,
  sanitizeMonitor,
  pickGpuDevices,
  parseHardwareJson,
  collectDeviceInfo,
} = require('./device-info.cjs')

// ─── fixtures ────────────────────────────────────────────────────────────────

const MONITOR_RAW = {
  instanceName: 'DISPLAY\\ACR1234\\5&1a2b3c&0&UID4352',
  manufacturer: 'ACR',
  model: 'XV275K',
  productCode: 'P275',
  year: 2024,
  week: 12,
  maxHcm: 60,
  maxVcm: 34,
  videoOutputTechnology: 5,
  diagonalInches: 999,
  serialNumber: 'SERIAL-LEAK-1',
  SerialNumberID: [83, 69, 82, 73, 65, 76],
  serial: 'SERIAL-LEAK-2',
  unknownKey: 'must be dropped',
}

const MONITOR_CLEAN = {
  instanceName: 'DISPLAY\\ACR1234\\5&1a2b3c&0&UID4352',
  manufacturer: 'ACR',
  model: 'XV275K',
  productCode: 'P275',
  year: 2024,
  week: 12,
  maxHcm: 60,
  maxVcm: 34,
  videoOutputTechnology: 5,
  diagonalInches: 27.2,
}

const GPU_DEVICE_RAW = {
  active: true,
  vendorId: 0x10de,
  deviceId: 0x2786,
  vendorString: 'NVIDIA Corporation',
  deviceString: 'NVIDIA GeForce RTX 4070',
  driverVersion: '31.0.15.5222',
  driverDate: '9-12-2023',
  driverVendor: 'NVIDIA',
  serialNumber: 'GPU-SERIAL-MUST-NOT-LEAK',
}

const GPU_DEVICE_CLEAN = {
  vendorId: 0x10de,
  deviceId: 0x2786,
  vendorString: 'NVIDIA Corporation',
  deviceString: 'NVIDIA GeForce RTX 4070',
  driverVersion: '31.0.15.5222',
  driverDate: '9-12-2023',
  active: true,
}

const WMI_PAYLOAD = {
  monitors: [MONITOR_RAW],
  gpus: [
    {
      name: 'NVIDIA GeForce RTX 4070',
      driverVersion: '31.0.15.5222',
      driverDate: '9-12-2023',
      adapterRam: 8589934592,
      width: 3840,
      height: 2160,
      refresh: 60,
    },
  ],
  registry: { hags: 2, mpo: 5, gameMode: 1 },
}

const PRIMARY_DISPLAY = {
  id: 100,
  label: 'Primary',
  bounds: { x: 0, y: 0, width: 3840, height: 2160 },
  workArea: { x: 0, y: 0, width: 3840, height: 2080 },
  size: { width: 3840, height: 2160 },
  scaleFactor: 1.5,
  rotation: 0,
  internal: false,
  touchSupport: 'unknown',
  colorDepth: 30,
  colorSpace: 'srgb',
  depthPerComponent: 10,
  displayFrequency: 60,
  nativeOrigin: { x: 0, y: 0 },
}

function fakeOsModule() {
  return {
    platform: () => 'win32',
    release: () => '10.0.26100',
    version: () => 'Windows 11 Pro',
    arch: () => 'x64',
    totalmem: () => 34359738368,
    cpus: () => [{ model: 'AMD Ryzen 9 7950X', speed: 4500 }],
  }
}

function fakeApp() {
  return {
    getName: () => 'Paiint',
    getVersion: () => '0.1.0',
    isPackaged: false,
    getLocale: () => 'en-GB',
    getGPUInfo: async (type) => {
      assert.equal(type, 'complete')
      return {
        gpuDevice: [GPU_DEVICE_RAW],
        machineModelName: 'XPS 8960',
        machineModelVersion: '1.0',
      }
    },
    getGPUFeatureStatus: async () => ({ gpu_compositing: 'enabled', webgl: 'enabled' }),
  }
}

function fakeScreen() {
  return {
    getAllDisplays: () => [PRIMARY_DISPLAY],
    getPrimaryDisplay: () => PRIMARY_DISPLAY,
  }
}

function fakeExecFile(payload = WMI_PAYLOAD) {
  const calls = []
  const execFile = (file, args, options, callback) => {
    calls.push({ file, args, options })
    callback(null, JSON.stringify(payload), '')
  }
  return { calls, execFile }
}

// ─── decodeWmiString ─────────────────────────────────────────────────────────

test('decodeWmiString decodes a WMI uint16 array and drops NUL padding', () => {
  assert.equal(decodeWmiString([65, 67, 82, 0, 0]), 'ACR')
})

test('decodeWmiString tolerates null, non-arrays and string input', () => {
  assert.equal(decodeWmiString(null), '')
  assert.equal(decodeWmiString(undefined), '')
  assert.equal(decodeWmiString(42), '')
  assert.equal(decodeWmiString({}), '')
  assert.equal(decodeWmiString([]), '')
  assert.equal(decodeWmiString('ACR'), 'ACR')
  assert.equal(decodeWmiString('ACR\0'), 'ACR')
})

// ─── monitorSizeInches ───────────────────────────────────────────────────────

test('monitorSizeInches converts centimetres to a one-decimal diagonal', () => {
  // sqrt(60^2 + 34^2) / 2.54 = 27.15... -> 27.2. (The brief's 27.5 example
  // is the diagonal of a 60 x 36 cm panel, covered below.)
  assert.equal(monitorSizeInches(60, 34), 27.2)
  assert.equal(monitorSizeInches(60, 36), 27.5)
})

test('monitorSizeInches rejects unusable dimensions', () => {
  assert.equal(monitorSizeInches(0, 34), null)
  assert.equal(monitorSizeInches(60, 0), null)
  assert.equal(monitorSizeInches(-60, 34), null)
  assert.equal(monitorSizeInches(Number.NaN, 34), null)
  assert.equal(monitorSizeInches(60, Number.POSITIVE_INFINITY), null)
  assert.equal(monitorSizeInches('60', 34), null)
  assert.equal(monitorSizeInches(undefined, undefined), null)
})

// ─── sanitizeMonitor ─────────────────────────────────────────────────────────

test('sanitizeMonitor keeps only whitelisted fields and drops every serial key', () => {
  const clean = sanitizeMonitor(MONITOR_RAW)

  assert.deepEqual(clean, MONITOR_CLEAN)
  for (const key of ['serialNumber', 'SerialNumberID', 'serial', 'unknownKey', 'diagonalInches']) {
    if (key === 'diagonalInches') {
      assert.equal(clean.diagonalInches, 27.2)
      continue
    }
    assert.equal(Object.hasOwn(clean, key), false, `${key} must not survive`)
  }
  assert.equal(JSON.stringify(clean).includes('SERIAL-LEAK'), false)
})

test('sanitizeMonitor computes a null diagonal when dimensions are absent', () => {
  const clean = sanitizeMonitor({ instanceName: 'DISPLAY\\DEL1234', model: 'U2720Q' })
  assert.deepEqual(clean, {
    instanceName: 'DISPLAY\\DEL1234',
    model: 'U2720Q',
    diagonalInches: null,
  })
})

test('sanitizeMonitor ignores non-objects', () => {
  assert.equal(sanitizeMonitor(null), null)
  assert.equal(sanitizeMonitor('nope'), null)
})

// ─── pickGpuDevices ──────────────────────────────────────────────────────────

test('pickGpuDevices returns only the safe GPU fields', () => {
  assert.deepEqual(pickGpuDevices({ gpuDevice: [GPU_DEVICE_RAW] }), [GPU_DEVICE_CLEAN])
  assert.equal(JSON.stringify(pickGpuDevices({ gpuDevice: [GPU_DEVICE_RAW] })).includes('GPU-SERIAL'), false)
})

test('pickGpuDevices returns an empty list when the array is missing', () => {
  assert.deepEqual(pickGpuDevices(undefined), [])
  assert.deepEqual(pickGpuDevices(null), [])
  assert.deepEqual(pickGpuDevices({}), [])
  assert.deepEqual(pickGpuDevices({ gpuDevice: 'nope' }), [])
})

// ─── parseHardwareJson ───────────────────────────────────────────────────────

test('parseHardwareJson normalises arrays, computes size and strips serials', () => {
  const parsed = parseHardwareJson(JSON.stringify(WMI_PAYLOAD))

  assert.deepEqual(parsed.monitors, [MONITOR_CLEAN])
  assert.deepEqual(parsed.gpus, WMI_PAYLOAD.gpus)
  assert.deepEqual(parsed.registry, WMI_PAYLOAD.registry)
  assert.equal(JSON.stringify(parsed.monitors).includes('SERIAL-LEAK'), false)
})

test('parseHardwareJson accepts a single monitor object and a missing registry', () => {
  const parsed = parseHardwareJson(JSON.stringify({
    monitors: {
      instanceName: 'DISPLAY\\DEL1234',
      manufacturer: 'DEL',
      model: 'U2720Q',
      maxHcm: 60,
      maxVcm: 36,
      serialNumber: 'SERIAL-LEAK-3',
    },
    gpus: { name: 'Intel UHD Graphics' },
  }))

  assert.equal(parsed.monitors.length, 1)
  assert.deepEqual(parsed.monitors[0], {
    instanceName: 'DISPLAY\\DEL1234',
    manufacturer: 'DEL',
    model: 'U2720Q',
    maxHcm: 60,
    maxVcm: 36,
    diagonalInches: 27.5,
  })
  assert.deepEqual(parsed.gpus, [{ name: 'Intel UHD Graphics' }])
  assert.deepEqual(parsed.registry, {})
  assert.equal(JSON.stringify(parsed.monitors).includes('SERIAL-LEAK'), false)
})

test('parseHardwareJson rejects invalid JSON with a clear error', () => {
  assert.throws(() => parseHardwareJson('{not json'), { message: 'invalid hardware JSON' })
  assert.throws(() => parseHardwareJson('null'), { message: 'invalid hardware JSON' })
})

// ─── collectDeviceInfo ───────────────────────────────────────────────────────

test('collectDeviceInfo returns the documented shape and calls PowerShell once', async () => {
  const { calls, execFile } = fakeExecFile()

  const info = await collectDeviceInfo({
    app: fakeApp(),
    screen: fakeScreen(),
    execFile,
    platform: 'win32',
    osModule: fakeOsModule(),
  })

  assert.equal(typeof info.collectedAt, 'string')
  assert.equal(Number.isNaN(Date.parse(info.collectedAt)), false)

  assert.equal(info.app.name, 'Paiint')
  assert.equal(info.app.version, '0.1.0')
  assert.equal(info.app.packaged, false)
  assert.equal(info.app.locale, 'en-GB')
  assert.equal(info.app.electron, process.versions.electron || null)
  assert.equal(info.app.chrome, process.versions.chrome || null)
  assert.equal(info.app.node, process.versions.node)
  assert.equal(info.app.v8, process.versions.v8)
  assert.equal(info.app.arch, process.arch)

  assert.deepEqual(info.os, {
    platform: 'win32',
    release: '10.0.26100',
    version: 'Windows 11 Pro',
    arch: 'x64',
    totalMemory: 34359738368,
    cpu: { model: 'AMD Ryzen 9 7950X', speed: 4500, count: 1 },
  })

  assert.deepEqual(info.displays, [PRIMARY_DISPLAY])
  assert.equal(info.primaryDisplayId, 100)

  assert.deepEqual(info.gpu, {
    devices: [GPU_DEVICE_CLEAN],
    machineModelName: 'XPS 8960',
    machineModelVersion: '1.0',
    featureStatus: { gpu_compositing: 'enabled', webgl: 'enabled' },
  })

  assert.deepEqual(info.hardware, {
    monitors: [MONITOR_CLEAN],
    gpus: WMI_PAYLOAD.gpus,
    registry: WMI_PAYLOAD.registry,
  })

  assert.equal(calls.length, 1)
  assert.equal(calls[0].file, 'powershell.exe')
  assert.deepEqual(calls[0].args.slice(0, 5), [
    '-NoProfile',
    '-NonInteractive',
    '-ExecutionPolicy',
    'Bypass',
    '-Command',
  ])
  assert.match(calls[0].args[5], /Get-CimInstance/)
  assert.match(calls[0].args[5], /WmiMonitorID/)
  assert.equal(calls[0].args[5].includes('SerialNumberID'), false)
  assert.equal(calls[0].options.windowsHide, true)
  assert.equal(calls[0].options.timeout, 10000)
  assert.equal(calls[0].options.maxBuffer, 4 * 1024 * 1024)
})

test('collectDeviceInfo records a WMI failure and keeps the rest of the document', async () => {
  const info = await collectDeviceInfo({
    app: fakeApp(),
    screen: fakeScreen(),
    execFile: (file, args, options, callback) => callback(new Error('wmi exploded')),
    platform: 'win32',
    osModule: fakeOsModule(),
  })

  assert.deepEqual(info.hardware, {
    monitors: [],
    gpus: [],
    registry: {},
    error: 'wmi exploded',
  })
  assert.equal(info.app.name, 'Paiint')
  assert.equal(info.displays.length, 1)
  assert.equal(info.gpu.devices.length, 1)
})

test('collectDeviceInfo records a GPU failure and keeps the rest of the document', async () => {
  const app = fakeApp()
  app.getGPUInfo = async () => {
    throw new Error('gpu exploded')
  }

  const info = await collectDeviceInfo({
    app,
    screen: fakeScreen(),
    execFile: fakeExecFile().execFile,
    platform: 'win32',
    osModule: fakeOsModule(),
  })

  assert.deepEqual(info.gpu.devices, [])
  assert.equal(info.gpu.error, 'gpu exploded')
  assert.equal(info.hardware.monitors.length, 1)
})

test('collectDeviceInfo survives a fake app and screen without optional APIs', async () => {
  const info = await collectDeviceInfo({
    app: {},
    screen: {},
    execFile: fakeExecFile().execFile,
    platform: 'win32',
    osModule: fakeOsModule(),
  })

  assert.deepEqual(info.gpu, {
    devices: [],
    machineModelName: null,
    machineModelVersion: null,
    featureStatus: null,
  })
  assert.deepEqual(info.displays, [])
  assert.equal(info.primaryDisplayId, null)
  assert.equal(info.app.name, null)
  assert.equal(info.app.version, null)
  assert.equal(info.app.packaged, null)
  assert.equal(info.hardware.monitors.length, 1)
})

test('collectDeviceInfo skips PowerShell and records not-windows elsewhere', async () => {
  let spawned = 0
  const info = await collectDeviceInfo({
    app: {},
    screen: {},
    execFile: () => {
      spawned += 1
    },
    platform: 'linux',
    osModule: fakeOsModule(),
  })

  assert.equal(spawned, 0)
  assert.deepEqual(info.hardware, {
    monitors: [],
    gpus: [],
    registry: {},
    error: 'not-windows',
  })
  assert.equal(info.os.platform, 'linux')
})

test('collectDeviceInfo works without an injected execFile off Windows', async () => {
  const info = await collectDeviceInfo({
    app: {},
    screen: {},
    platform: 'linux',
    osModule: fakeOsModule(),
  })

  assert.equal(info.hardware.error, 'not-windows')
})
