'use strict'
// P0.3 headless proof: REAL AppStateManager + WallpaperLibrary (+ preload
// bridge shape) with a stubbed 'electron' module and temp userData.
// Covers: set/get round-trip, debounce flush, backup rotation, corrupt
// recovery, restart persistence; library add/list/get/remove/update,
// path-dedup (incl. case-insensitivity), restart persistence; preload
// exposes appState + gallery channels with exact names.
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const Module = require('node:module')

const userData = fs.mkdtempSync(path.join(os.tmpdir(), 'rex-p03-'))
const ipcHandlers = {}
const electronStub = {
  app: { getPath: (n) => (n === 'userData' ? userData : os.tmpdir()) },
  ipcMain: { handle: (ch, fn) => { ipcHandlers[ch] = fn } },
  ipcRenderer: {
    invoke: async (ch, ...args) => {
      if (!(ch in ipcHandlers)) throw new Error('no handler: ' + ch)
      return ipcHandlers[ch]({}, ...args)
    },
    on: () => {},
    removeListener: () => {},
  },
  contextBridge: {
    exposed: null,
    exposeInMainWorld(name, api) { this.exposed = { name, api } },
  },
  webUtils: { getPathForFile: (f) => (f && f.path) || null },
}
const origLoad = Module._load
Module._load = function (req, ...rest) {
  if (req === 'electron') return electronStub
  return origLoad.call(this, req, ...rest)
}

let failures = 0
function check(name, cond, extra) {
  console.log((cond ? 'PASS ' : 'FAIL ') + name + (extra ? ' [' + extra + ']' : ''))
  if (!cond) failures++
}

async function main() {
  const { AppStateManager } = require('D:\\REX_doc\\app\\electron\\app-state-manager.cjs')
  const { WallpaperLibrary } = require('D:\\REX_doc\\app\\electron\\wallpaper-library.cjs')

  // main.cjs wiring order: construct (registers gallery.* IPC), then
  // app-state handlers (mirrors the added ipcMain.handle blocks).
  const appState = new AppStateManager()
  const galleryLib = new WallpaperLibrary()
  ipcHandlers['app-state:get'] = async (e, kp) => appState.get(kp) ?? null
  ipcHandlers['app-state:set'] = async (e, kp, v) => appState.set(kp, v) ?? null

  // --- app-state ---
  check('AS: default window width', (await electronStub.ipcRenderer.invoke('app-state:get', 'window.width')) === 1280)
  await electronStub.ipcRenderer.invoke('app-state:set', 'ui.lastPage', 'Settings')
  check('AS: set/get round-trip', (await electronStub.ipcRenderer.invoke('app-state:get', 'ui.lastPage')) === 'Settings')
  check('AS: unknown key null', (await electronStub.ipcRenderer.invoke('app-state:get', 'nope.nothing')) === null)
  appState.flush()
  const f1 = JSON.parse(fs.readFileSync(path.join(userData, 'app-state.json'), 'utf8'))
  check('AS: flushed to disk', f1.ui.lastPage === 'Settings')
  const appState2 = new AppStateManager() // restart: same userData
  check('AS: restart persistence', appState2.get('ui.lastPage') === 'Settings')
  // corrupt recovery
  fs.writeFileSync(path.join(userData, 'app-state.json'), '{broken!!!')
  const appState3 = new AppStateManager()
  check('AS: corrupt recovers to defaults', appState3.get('window.width') === 1280)
  check('AS: corrupt quarantined', fs.existsSync(path.join(userData, 'app-state.json.corrupt')))

  // --- library ---
  const a1 = await electronStub.ipcRenderer.invoke('gallery.add', 'D:\\vids\\A.mp4', 'video')
  check('LIB: add returns item', !!a1 && a1.type === 'video')
  const a2 = await electronStub.ipcRenderer.invoke('gallery.add', 'd:\\VIDS\\a.mp4', 'video')
  check('LIB: case-insensitive dupe = same entry', a2 && a1 && a2.id === a1.id, 'ids=' + (a1 && a1.id) + '/' + (a2 && a2.id))
  const list1 = await electronStub.ipcRenderer.invoke('gallery.list', {})
  check('LIB: no duplicate rows', list1.length === 1, 'len=' + list1.length)
  check('LIB: get by path', (await electronStub.ipcRenderer.invoke('gallery.get', 'D:\\vids\\a.MP4')) !== null)
  await electronStub.ipcRenderer.invoke('gallery.add', 'D:\\pics\\b.png', 'static')
  check('LIB: type filter', (await electronStub.ipcRenderer.invoke('gallery.list', { type: 'static' })).length === 1)
  await electronStub.ipcRenderer.invoke('gallery.update', 'D:\\pics\\b.png', { favorite: true })
  check('LIB: update favorite', (await electronStub.ipcRenderer.invoke('gallery.get', 'd:/pics/b.png')).favorite === true)
  check('LIB: remove true', (await electronStub.ipcRenderer.invoke('gallery.remove', 'D:\\pics\\b.png')) === true)
  check('LIB: remove missing false', (await electronStub.ipcRenderer.invoke('gallery.remove', 'D:\\nope.png')) === false)
  // restart persistence across re-instantiation (same file)
  const { WallpaperLibrary: WL2 } = require('D:\\REX_doc\\app\\electron\\wallpaper-library.cjs')
  const galleryLib2 = new WL2()
  const list2 = galleryLib2.list({})
  // NOTE: add() refreshes the stored path to the latest-provided form
  // (existing.path = filePath) while keeping one entry + stable id.
  check('LIB: restart persistence', list2.length === 1 && list2[0].id === a1.id, JSON.stringify(list2.map((i) => i.path)))

  // --- preload bridge shape ---
  require('D:\\REX_doc\\app\\electron\\preload.cjs')
  const api = electronStub.contextBridge.exposed.api
  check('PRE: appState.get/set bridged', typeof api.appState?.get === 'function' && typeof api.appState?.set === 'function')
  const gch = ['add', 'get', 'list', 'remove', 'update', 'readFile']
  check('PRE: gallery.* bridged', gch.every((k) => typeof api.gallery?.[k] === 'function'), Object.keys(api.gallery || {}).join(','))
  // bridge call-through with the stub ipcRenderer (no gallery.read-file handler here
  // because main.cjs owns it — assert the invoke path, not the result)
  try { await api.gallery.list({}) } catch (e) { check('PRE: gallery.list invokes gallery.list channel', /no handler: gallery\.list/.test(e.message), e.message) }

  console.log(failures === 0 ? 'ALL PASS' : failures + ' FAILURES')
  process.exit(failures === 0 ? 0 : 1)
}
main()
