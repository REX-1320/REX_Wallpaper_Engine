import { useSyncExternalStore } from 'react'
import { detectFileType } from '../registry'
import { createPlaylist, deletePlaylist as deleteStorePlaylist, getSnapshot, renamePlaylist as renameStorePlaylist, restorePlaylists, selectWallpaper, updateWallpaperProperty, duplicatePlaylist as duplicateStorePlaylist } from '../store'
import { getMedia } from '../mediaBridge'

export const CHANGE_MODES = [
  'Manual',
  'Timer',
  'After Current Ends',
  'Video Ends',
  'Audio Ends',
  'Daily Schedule',
  'Weekly Schedule',
  'Monthly Schedule',
  'Startup',
  'Idle Time',
  'Lock Screen',
  'Unlock',
  'Battery Level',
  'Charging',
  'CPU Usage',
  'GPU Usage',
  'RAM Usage',
  'Temperature',
  'Weather',
  'Sunrise',
  'Sunset',
  'Random Interval',
  'Custom Rule',
]

export const PLAYBACK_ORDERS = [
  'Sequential',
  'Random',
  'Shuffle',
  'Repeat All',
  'Repeat One',
  'Weighted Random',
  'Recently Added',
  'Oldest First',
  'Newest First',
  'Alphabetical',
  'Custom Order',
  'Favorites First',
  'Highest Rated',
  'Least Played',
  'Most Played',
]

export const TRANSITIONS = [
  'None',
  'Fade',
  'Crossfade',
  'Slide',
  'Zoom',
  'Blur',
  'Spiral',
  'Ripple',
  'Wipe',
  'Circle',
  'Smear',
  'Morph',
  'Glitch',
  'Pixelate',
  'Vortex',
  'Distort',
  'Rise',
  'Fall',
  'Curtain',
  'Flip',
  'Tunnel',
  'Particle',
  'Starfield',
  'Custom',
]

export const TRANSITION_CATEGORIES = {
  Basic: ['None', 'Fade', 'Crossfade', 'Slide', 'Zoom', 'Wipe', 'Circle', 'Curtain', 'Flip'],
  Movement: ['Rise', 'Fall', 'Spiral', 'Vortex', 'Tunnel'],
  Blur: ['Blur', 'Pixelate', 'Smear'],
  Distortion: ['Distort', 'Ripple', 'Morph'],
  Particles: ['Particle', 'Starfield'],
  Shader: ['Glitch'],
  '3D': ['Flip', 'Tunnel'],
  Custom: ['Custom'],
}

export const TRANSITION_CURVES = ['Linear', 'Ease In', 'Ease Out', 'Ease In Out', 'Spring', 'Custom Bezier']

export const STARTUP_OPTIONS = {
  resumePrevious: 'Resume previous wallpaper',
  alwaysFirst: 'Always begin with first wallpaper',
  continueFromPosition: 'Continue from last position',
  shuffleOnStartup: 'Shuffle on startup',
  randomOnStartup: 'Random wallpaper on startup',
  restorePlaylist: 'Restore previous playlist',
}

export const BEHAVIOR_OPTIONS = {
  allowWhilePaused: 'Allow wallpaper changes while paused',
  pauseOnFullscreen: 'Pause when fullscreen application starts',
  pauseOnGame: 'Pause when game launches',
  pauseOnBatterySaver: 'Pause on battery saver',
  pauseOnRemoteDesktop: 'Pause on remote desktop',
  pauseWhilePresenting: 'Pause while presenting',
  pauseDuringVideoPlayback: 'Pause during video playback',
  pauseWhileScreenSharing: 'Pause while screen sharing',
  pauseOnLidClose: 'Pause when laptop lid closes',
  continueAudioWhilePaused: 'Continue audio while paused',
}

export const AUTOMATION_TRIGGERS = [
  'Time',
  'Battery',
  'Power Mode',
  'Weather',
  'Location',
  'Display Configuration',
  'Connected Monitor',
  'Docked State',
  'Gaming Mode',
  'Focus Mode',
  'Sleep Mode',
  'Work Hours',
  'Weekend',
  'Holiday',
]

export const MULTIMONITOR_OPTIONS = {
  perMonitor: 'Separate playlists per monitor',
  mirror: 'Mirror playlist',
  independentTimers: 'Independent timers',
  synchronized: 'Synchronized playback',
  independentTransitions: 'Independent transitions',
  independentRandomization: 'Independent randomization',
  span: 'Span playlist',
}

export const FILTER_INCLUDES = {
  IMAGE: 'Images',
  VIDEO: 'Videos',
  WEB: 'Web',
  SHADER: 'Shaders',
  SCENE: 'Scene',
  AUDIO_VISUALIZER: 'Visualizer',
}

export const FILTER_KEYS = ['tags', 'author', 'rating', 'resolution', 'fps', 'duration', 'renderer', 'collection', 'folder', 'favorite', 'recentlyAdded']

export const SMART_RULES = {
  skipNsfw: 'Skip NSFW',
  skipLargeFiles: 'Skip Large Files',
  skipHighGpu: 'Skip High GPU Wallpapers',
  skipHighCpu: 'Skip High CPU Wallpapers',
  onlyHdr: 'Only HDR Wallpapers',
  only4k: 'Only 4K Wallpapers',
  onlyAnimated: 'Only Animated Wallpapers',
  onlyStatic: 'Only Static Wallpapers',
  onlyAudio: 'Only Audio Wallpapers',
  onlyFavorites: 'Only Favorite Wallpapers',
}

export const PLAYLIST_OPTIONS = {
  changeAfterVideoEnds: 'Change wallpaper after video ends',
  changeAfterImageTimeout: 'Change wallpaper after image timeout',
  pause: 'Pause playlist',
  restart: 'Restart playlist',
  loopPlaylist: 'Loop playlist',
  loopCurrent: 'Loop current wallpaper',
  skipUnavailable: 'Skip unavailable wallpapers',
  skipUnsupportedRenderers: 'Skip unsupported renderers',
  autoRemoveMissing: 'Automatically remove missing files',
  validateBeforePlayback: 'Validate playlist before playback',
}

const STORAGE_KEY = 'rex.playlistEngine.v1'

function deepDefault() {
  return {
    mode: 'Manual',
    modeSettings: {
      timer: { days: 0, hours: 0, minutes: 1, seconds: 0, ms: 0, running: true, accumulatedMs: 0, startedAt: Date.now(), pausedAt: null },
      dailyTime: '21:00',
      weeklyDay: 'Monday',
      weeklyTime: '21:00',
      monthlyDay: 1,
      monthlyTime: '21:00',
      idleMinutes: 10,
      batteryLevel: 20,
      threshold: 80,
      temperature: 60,
      weatherTarget: 'Rainy',
      randomMin: 60,
      randomMax: 600,
      customRuleSource: 'return true',
      sunrise: '06:00',
      sunset: '18:30',
      firedAt: 0,
      endedHandled: false,
      startupFired: false,
      customOrder: [],
    },
    order: 'Sequential',
    transition: 'Fade',
    transitionMultiple: false,
    transitionsEnabled: ['Fade'],
    transitionFavoritesOnly: false,
    transitionFavorites: ['Fade'],
    transitionRandom: false,
    transitionSmart: false,
    transitionPerWallpaper: false,
    transitionGlobal: true,
    transitionDuration: 800,
    transitionCurve: 'Ease In Out',
    options: {
      changeAfterVideoEnds: true,
      changeAfterImageTimeout: false,
      pause: false,
      restart: false,
      loopPlaylist: true,
      loopCurrent: false,
      skipUnavailable: true,
      skipUnsupportedRenderers: true,
      autoRemoveMissing: false,
      validateBeforePlayback: true,
    },
    startup: { resumePrevious: true, alwaysFirst: false, continueFromPosition: false, shuffleOnStartup: false, randomOnStartup: false, restorePlaylist: true },
    behavior: Object.fromEntries(Object.keys(BEHAVIOR_OPTIONS).map((k) => [k, false])),
    automation: { enabled: false, trigger: 'Time', switchTo: '', time: '21:00', batteryBelow: 20, powerMode: 'Battery', weather: 'Rainy', location: '', displayConfig: 'Monitor 1', dockedState: 'Docked' },
    multiMonitor: Object.fromEntries(Object.keys(MULTIMONITOR_OPTIONS).map((k) => [k, false])),
    filters: {
      include: { IMAGE: true, VIDEO: true, WEB: true, SHADER: true, SCENE: true, AUDIO_VISUALIZER: true },
      tags: '',
      author: '',
      minRating: 0,
      minResolution: '',
      maxFps: 0,
      maxDuration: 0,
      renderer: '',
      collection: '',
      folder: '',
      favorite: false,
      recentlyAdded: false,
    },
    rules: Object.fromEntries(Object.keys(SMART_RULES).map((k) => [k, false])),
    lastChangeAt: Date.now(),
  }
}

let settingsByPlaylist = {}
let presets = []
let history = { recent: [], mostPlayed: {}, skipped: [], failed: [], playbackLog: [], transitionLog: [] }
let defaultTemplate = null

function mergeDeep(base, override) {
  const out = { ...base }
  for (const [key, value] of Object.entries(override ?? {})) {
    if (value && typeof value === 'object' && !Array.isArray(value) && base[key] && typeof base[key] === 'object') {
      out[key] = mergeDeep(base[key], value)
    } else {
      out[key] = value
    }
  }
  return out
}

const state = {
  now: Date.now(),
  idleSince: Date.now(),
  lastChangeAt: Date.now(),
  system: { cpu: 0, gpu: 0, ram: 0, battery: null, charging: null, docked: false, fullscreen: false, game: false, presenting: false, screenSharing: false, remoteDesktop: false, lidClosed: false, lockScreen: false },
  weather: 'Clear',
  temperature: null,
  sunTimes: null,
  currentPlaylistId: null,
}

let version = 0
const listeners = new Set()
let hookSnapshot = null
let timer = null
let systemTimer = null
let started = false

function bump() {
  version += 1
  state.now = Date.now()
  hookSnapshot = null
  for (const listener of listeners) listener()
  persist()
}

function persist() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ settingsByPlaylist, presets, history, defaultTemplate }))
  } catch {
    // ignore quota / unavailable storage
  }
}

function load() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (!raw) return
    const parsed = JSON.parse(raw)
    if (parsed.settingsByPlaylist) settingsByPlaylist = parsed.settingsByPlaylist
    if (Array.isArray(parsed.presets)) presets = parsed.presets
    if (parsed.history) history = parsed.history
    if (parsed.defaultTemplate) defaultTemplate = parsed.defaultTemplate
  } catch {
    // corrupt storage -> start fresh
  }
}

function getSettings(playlistId) {
  if (!playlistId) return null
  if (!settingsByPlaylist[playlistId]) {
    settingsByPlaylist[playlistId] = defaultTemplate ? mergeDeep(deepDefault(), defaultTemplate) : deepDefault()
  }
  return settingsByPlaylist[playlistId]
}

function updateSettings(playlistId, updater) {
  const settings = getSettings(playlistId)
  if (!settings) return
  updater(settings)
  bump()
}

function setPath(target, path, value) {
  let node = target
  for (let i = 0; i < path.length - 1; i += 1) node = node[path[i]]
  node[path[path.length - 1]] = value
}

function poolWallpapers() {
  const snapshot = getSnapshot()
  return snapshot.wallpapers ?? []
}

function activePlaylistId() {
  const snapshot = getSnapshot()
  const selected = snapshot.wallpapers.find((w) => w.id === snapshot.selectedId)
  if (!selected) return null
  return (snapshot.playlists ?? []).find((p) => p.name === selected.playlist.currentPlaylist)?.id ?? null
}

function filterPool(settings, pool) {
  const { filters, rules } = settings
  return pool.filter((w) => {
    const type = detectFileType(w.info.source ?? '') ?? 'IMAGE'
    if (filters.include[type] === false) return false
    if (filters.favorite && !w.general?.favorite) return false
    if (filters.recentlyAdded && !w.info?.dateAdded) return false
    if (filters.minRating > 0 && (w.general?.rating ?? 0) < filters.minRating) return false
    if (filters.maxFps > 0) {
      const fl = parseInt(w.performance?.frameLimit ?? '', 10)
      if (!Number.isNaN(fl) && fl > filters.maxFps) return false
    }
    if (filters.maxDuration > 0) {
      const duration = w.media?.duration ?? 0
      if (duration > 0 && duration > filters.maxDuration) return false
    }
    if (filters.renderer && w.performance?.renderer && w.performance.renderer !== filters.renderer) return false
    if (filters.author && !(w.info?.author ?? '').toLowerCase().includes(filters.author.toLowerCase())) return false
    if (filters.tags && !(w.info?.tags ?? []).some((t) => t.toLowerCase().includes(filters.tags.toLowerCase()))) return false
    if (rules.skipNsfw && w.info?.nsfw) return false
    if (rules.skipLargeFiles && (w.info?.fileSize ?? '').includes('MB') && parseFloat(w.info.fileSize) > 50) return false
    if (rules.skipHighGpu && w.performance?.renderer === 'CPU') return false
    if (rules.skipHighCpu && w.performance?.renderer === 'CPU') return false
    if (rules.onlyHdr && !w.renderer?.hdr) return false
    if (rules.only4k && !(w.info?.resolution ?? '').includes('4K') && !(w.info?.resolution ?? '').includes('3840')) return false
    if (rules.onlyAnimated && type === 'IMAGE') return false
    if (rules.onlyStatic && type !== 'IMAGE') return false
    if (rules.onlyFavorites && !w.general?.favorite) return false
    return true
  })
}

function pickNext(settings, pool, currentId) {
  if (pool.length === 0) return null
  if (settings.order === 'Repeat One' || settings.options.loopCurrent) {
    return pool.find((w) => w.id === currentId) ?? pool[0]
  }
  const currentIndex = pool.findIndex((w) => w.id === currentId)
  switch (settings.order) {
    case 'Random':
      return pool[Math.floor(Math.random() * pool.length)]
    case 'Shuffle': {
      let order = settings._shuffleOrder
      if (!order || order.length === 0) order = shuffleArray(pool.map((w) => w.id))
      const next = order.shift()
      settings._shuffleOrder = order
      return pool.find((w) => w.id === next) ?? pool[0]
    }
    case 'Weighted Random': {
      const weights = pool.map((w) => {
        let weight = 1
        if (w.general?.favorite) weight += 3
        if (w.info?.dateAdded) weight += 1
        return weight
      })
      const total = weights.reduce((a, b) => a + b, 0)
      let roll = Math.random() * total
      for (let i = 0; i < pool.length; i += 1) {
        roll -= weights[i]
        if (roll <= 0) return pool[i]
      }
      return pool[pool.length - 1]
    }
    case 'Recently Added':
    case 'Newest First':
      return sortByDate(pool, true)[0]
    case 'Oldest First':
      return sortByDate(pool, false)[0]
    case 'Alphabetical':
      return sortByName(pool, true)[0]
    case 'Custom Order': {
      const ordered = settings.modeSettings.customOrder.filter((id) => pool.some((w) => w.id === id))
      const rest = pool.filter((w) => !settings.modeSettings.customOrder.includes(w.id))
      const list = [...ordered, ...rest]
      const nextIndex = (list.findIndex((w) => w.id === currentId) + 1) % list.length
      return list[nextIndex]
    }
    case 'Favorites First': {
      const favs = pool.filter((w) => w.general?.favorite)
      const rest = pool.filter((w) => !w.general?.favorite)
      return [...favs, ...rest][0]
    }
    case 'Highest Rated':
      return [...pool].sort((a, b) => (b.general?.rating ?? 0) - (a.general?.rating ?? 0))[0]
    case 'Least Played':
      return [...pool].sort((a, b) => (history.mostPlayed[a.id] ?? 0) - (history.mostPlayed[b.id] ?? 0))[0]
    case 'Most Played':
      return [...pool].sort((a, b) => (history.mostPlayed[b.id] ?? 0) - (history.mostPlayed[a.id] ?? 0))[0]
    case 'Sequential':
    case 'Repeat All':
    default: {
      const nextIndex = (currentIndex + 1) % pool.length
      return pool[nextIndex]
    }
  }
}

function sortByDate(pool, newestFirst) {
  return [...pool].sort((a, b) => {
    const da = a.info?.dateAdded ?? ''
    const db = b.info?.dateAdded ?? ''
    return newestFirst ? db.localeCompare(da) : da.localeCompare(db)
  })
}

function sortByName(pool, asc) {
  return [...pool].sort((a, b) => {
    const na = a.info?.name ?? ''
    const nb = b.info?.name ?? ''
    return asc ? na.localeCompare(nb) : nb.localeCompare(na)
  })
}

function shuffleArray(arr) {
  const copy = [...arr]
  for (let i = copy.length - 1; i > 0; i -= 1) {
    const j = Math.floor(Math.random() * (i + 1))
    ;[copy[i], copy[j]] = [copy[j], copy[i]]
  }
  return copy
}

function transitionName(settings, wallpaper) {
  if (settings.transitionPerWallpaper && wallpaper?.playlist?.transition && wallpaper.playlist.transition !== 'None' && wallpaper.playlist.transition !== 'Global') {
    return wallpaper.playlist.transition
  }
  if (settings.transitionRandom) {
    const pool = settings.transitionFavoritesOnly ? settings.transitionFavorites : TRANSITIONS.filter((t) => t !== 'None')
    if (pool.length === 0) return settings.transition
    return pool[Math.floor(Math.random() * pool.length)]
  }
  if (settings.transitionSmart) {
    const type = detectFileType(wallpaper?.info?.source ?? '') ?? 'IMAGE'
    if (type === 'IMAGE') return 'Fade'
    if (type === 'VIDEO') return 'Crossfade'
    return 'Zoom'
  }
  if (settings.transitionMultiple) {
    const enabled = settings.transitionsEnabled.filter((t) => t !== 'None')
    if (enabled.length === 0) return settings.transition
    return enabled[Math.floor(Math.random() * enabled.length)]
  }
  return settings.transition
}

function fireChange(settings, reason) {
  const snapshot = getSnapshot()
  const selected = snapshot.wallpapers.find((w) => w.id === snapshot.selectedId)
  const activeName = selected?.playlist?.currentPlaylist ?? null
  const active = (snapshot.playlists ?? []).find((p) => p.name === activeName)
  const pool = filterPool(settings, poolWallpapers())
  if (activeName && activeName !== 'None' && active) {
    const changeNow = () => {
      const next = pickNext(settings, pool, snapshot.selectedId)
      if (!next) return
      const transition = transitionName(settings, next)
      recordPlay(next, transition)
      settings.lastChangeAt = Date.now()
      selectWallpaper(next.id)
    }
    if (settings.options.validateBeforePlayback) {
      const issues = validatePlaylist(pool)
      if (issues.length > 0) history.failed.push({ t: Date.now(), reason: 'validation', issues: issues.slice(0, 3) })
    }
    changeNow()
  }
}

function recordPlay(wallpaper, transition) {
  history.recent = [wallpaper.id, ...history.recent.filter((id) => id !== wallpaper.id)].slice(0, 50)
  history.mostPlayed[wallpaper.id] = (history.mostPlayed[wallpaper.id] ?? 0) + 1
  history.playbackLog.unshift({ t: Date.now(), id: wallpaper.id, name: wallpaper.info?.name ?? 'Wallpaper', type: detectFileType(wallpaper.info?.source ?? '') ?? 'IMAGE' })
  history.playbackLog = history.playbackLog.slice(0, 200)
  if (transition && transition !== 'None') {
    history.transitionLog.unshift({ t: Date.now(), from: null, to: wallpaper.info?.name ?? '', transition })
    history.transitionLog = history.transitionLog.slice(0, 100)
  }
}

function timerTotalMs(settings) {
  const t = settings.modeSettings.timer
  return (((t.days * 24 + t.hours) * 60 + t.minutes) * 60 + t.seconds) * 1000 + t.ms
}

function timerRemainingMs(settings) {
  const t = settings.modeSettings.timer
  const total = timerTotalMs(settings)
  if (total <= 0) return 0
  const elapsed = t.accumulatedMs + (t.running ? state.now - t.startedAt : 0)
  return Math.max(0, total - elapsed)
}

function evaluateMode(settings) {
  const ms = settings.modeSettings
  const now = state.now
  const mode = settings.mode
  if (settings.options.pause) return null
  for (const [key, enabled] of Object.entries(settings.behavior)) {
    const signal = {
      pauseOnFullscreen: state.system.fullscreen,
      pauseOnGame: state.system.game,
      pauseOnBatterySaver: state.system.battery !== null && state.system.battery <= 20 && !state.system.charging,
      pauseOnRemoteDesktop: state.system.remoteDesktop,
      pauseWhilePresenting: state.system.presenting,
      pauseDuringVideoPlayback: state.system.fullscreen,
      pauseWhileScreenSharing: state.system.screenSharing,
      pauseOnLidClose: state.system.lidClosed,
    }[key]
    if (enabled && signal) return null
  }

  if (mode === 'Timer') {
    if (timerTotalMs(settings) <= 0) return null
    if (timerRemainingMs(settings) <= 0) {
      ms.timer.accumulatedMs = 0
      ms.timer.startedAt = now
      ms.timer.running = true
      return 'Timer elapsed'
    }
    return null
  }
  if (mode === 'After Current Ends' || mode === 'Video Ends' || mode === 'Audio Ends') {
    const snapshot = getSnapshot()
    const el = getMedia(snapshot.selectedId)
    const ended = el ? el.ended : false
    if (ended && !ms.endedHandled) {
      ms.endedHandled = true
      return mode
    }
    if (!ended) ms.endedHandled = false
    return null
  }
  if (mode === 'Daily Schedule') return timeMatch(ms.dailyTime, now) && firedOnce(ms, now)
  if (mode === 'Weekly Schedule') {
    const days = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday']
    const today = days[new Date(now).getDay()]
    return today === ms.weeklyDay && timeMatch(ms.weeklyTime, now) && firedOnce(ms, now)
  }
  if (mode === 'Monthly Schedule') {
    return new Date(now).getDate() === ms.monthlyDay && timeMatch(ms.monthlyTime, now) && firedOnce(ms, now)
  }
  if (mode === 'Startup') {
    if (!ms.startupFired) {
      ms.startupFired = true
      return 'Startup'
    }
    return null
  }
  if (mode === 'Idle Time') {
    if (now - state.idleSince >= ms.idleMinutes * 60000) {
      state.idleSince = now
      return 'Idle Time'
    }
    return null
  }
  if (mode === 'Lock Screen') {
    if (state.system.lockScreen && !ms.lockHandled) {
      ms.lockHandled = true
      return 'Lock Screen'
    }
    if (!state.system.lockScreen) ms.lockHandled = false
    return null
  }
  if (mode === 'Unlock') {
    if (state.system.lockScreen) ms.lockHandled = true
    else if (ms.lockHandled) {
      ms.lockHandled = false
      return 'Unlock'
    }
    return null
  }
  if (mode === 'Battery Level' && state.system.battery !== null) {
    const below = state.system.battery <= ms.batteryLevel && !state.system.charging
    if (below && !ms.batteryCrossed) {
      ms.batteryCrossed = true
      return 'Battery Level'
    }
    if (!below) ms.batteryCrossed = false
    return null
  }
  if (mode === 'Charging') {
    if (state.system.charging && !ms.chargingHandled) {
      ms.chargingHandled = true
      return 'Charging'
    }
    if (!state.system.charging) ms.chargingHandled = false
    return null
  }
  if (mode === 'CPU Usage' || mode === 'GPU Usage' || mode === 'RAM Usage') {
    const key = mode === 'CPU Usage' ? 'cpu' : mode === 'GPU Usage' ? 'gpu' : 'ram'
    const above = state.system[key] >= ms.threshold
    if (above && !ms.usageCrossed) {
      ms.usageCrossed = true
      return mode
    }
    if (!above) ms.usageCrossed = false
    return null
  }
  if (mode === 'Temperature') {
    const above = state.temperature !== null && state.temperature >= ms.temperature
    if (above && !ms.tempCrossed) {
      ms.tempCrossed = true
      return 'Temperature'
    }
    if (!above) ms.tempCrossed = false
    return null
  }
  if (mode === 'Weather') {
    if (state.weather === ms.weatherTarget && state.weather !== ms.lastWeather) {
      ms.lastWeather = state.weather
      return 'Weather'
    }
    if (state.weather !== ms.weatherTarget) ms.lastWeather = null
    return null
  }
  if (mode === 'Sunrise' && timeMatch(ms.sunrise, now) && firedOnce(ms, now)) return 'Sunrise'
  if (mode === 'Sunset' && timeMatch(ms.sunset, now) && firedOnce(ms, now)) return 'Sunset'
  if (mode === 'Random Interval') {
    const min = ms.randomMin * 1000
    const max = ms.randomMax * 1000
    const interval = min + Math.random() * (max - min)
    if (now - settings.lastChangeAt >= interval) {
      settings.lastChangeAt = now
      return 'Random Interval'
    }
    return null
  }
  if (mode === 'Custom Rule') {
    try {
      const fn = new Function('ctx', `with (ctx) { ${ms.customRuleSource} }`)
      const ctx = { battery: state.system.battery, charging: state.system.charging, cpu: state.system.cpu, gpu: state.system.gpu, ram: state.system.ram, temperature: state.system.temperature, weather: state.weather, hour: new Date(now).getHours(), minute: new Date(now).getMinutes(), day: new Date(now).getDay(), date: new Date(now).getDate() }
      const result = Boolean(fn(ctx))
      if (result && !ms.ruleFired) {
        ms.ruleFired = true
        ms.firedAt = now
        return 'Custom Rule'
      }
      if (!result) ms.ruleFired = false
    } catch {
      return null
    }
    return null
  }
  return null
}

function firedOnce(ms, now) {
  const dayKey = new Date(now).toDateString()
  if (ms.firedAt && new Date(ms.firedAt).toDateString() === dayKey) return null
  ms.firedAt = now
  return 'Schedule'
}

function timeMatch(time, now) {
  if (!time) return false
  const [h, m] = time.split(':').map(Number)
  const d = new Date(now)
  return d.getHours() === h && d.getMinutes() === m
}

function tick() {
  const id = activePlaylistId()
  if (id && id !== state.currentPlaylistId) {
    state.currentPlaylistId = id
    const settings = getSettings(id)
    if (settings.startup.randomOnStartup) fireChange(settings, 'Startup')
  }
  const settings = state.currentPlaylistId ? getSettings(state.currentPlaylistId) : null
  if (settings) {
    const reason = evaluateMode(settings)
    if (reason) fireChange(settings, reason)
    if (settings.automation.enabled && evaluateAutomation(settings)) {
      const target = (getSnapshot().playlists ?? []).find((p) => p.name === settings.automation.switchTo)
      if (target) {
        const snapshot = getSnapshot()
        const selected = snapshot.wallpapers.find((w) => w.id === snapshot.selectedId)
        if (selected) {
          updateWallpaperProperty(selected.id, ['playlist', 'currentPlaylist'], target.name)
        }
      }
    }
  }
  bump()
}

function evaluateAutomation(settings) {
  const a = settings.automation
  if (!a.switchTo) return false
  const now = state.now
  const d = new Date(now)
  switch (a.trigger) {
    case 'Time':
      return timeMatch(a.time, now)
    case 'Battery':
      return state.system.battery !== null && state.system.battery <= a.batteryBelow
    case 'Power Mode':
      return state.system.battery !== null && (a.powerMode === 'Battery' ? !state.system.charging : state.system.charging)
    case 'Weather':
      return state.weather === a.weather
    case 'Work Hours': {
      const [start, end] = a.time.split('-')
      const hour = d.getHours()
      const minutes = d.getHours() * 60 + d.getMinutes()
      if (start && end) {
        const [sh, sm] = start.split(':').map(Number)
        const [eh, em] = end.split(':').map(Number)
        return minutes >= sh * 60 + sm && minutes <= eh * 60 + em
      }
      return hour >= 9 && hour < 17
    }
    case 'Weekend':
      return d.getDay() === 0 || d.getDay() === 6
    case 'Docked State':
      return state.system.docked === (a.dockedState === 'Docked')
    case 'Display Configuration':
      return state.system.fullscreen === (a.displayConfig === 'Fullscreen')
    default:
      return false
  }
}

function validatePlaylist(pool) {
  const issues = []
  for (const w of pool) {
    const missing = !w.fileUrl && !w.webUrl && !w.sceneData && !w.shaderSource && !w.audioSource && !w.info?.source
    if (missing) issues.push({ id: w.id, name: w.info?.name ?? 'Unknown', reason: 'missing file' })
    if (w.performance?.renderer === 'CPU' && w.info?.type === 'SHADER') issues.push({ id: w.id, name: w.info?.name ?? 'Unknown', reason: 'unsupported renderer' })
  }
  return issues
}

export const playlistEngine = {
  start() {
    load()
    if (started) return
    started = true
    state.idleSince = Date.now()
    timer = setInterval(tick, 500)
    systemTimer = setInterval(() => {
      const memory = performance.memory ?? null
      const ram = memory ? Math.round((memory.usedJSHeapSize / memory.jsHeapSizeLimit) * 100) : 0
      state.system.cpu = Math.min(100, 20 + Math.round(Math.random() * 30))
      state.system.gpu = Math.min(100, state.system.cpu - Math.round(Math.random() * 20))
      state.system.ram = ram
      if (navigator.getBattery) {
        navigator.getBattery().then((battery) => {
          state.system.battery = Math.round(battery.level * 100)
          state.system.charging = battery.charging
        }).catch(() => {})
      }
    }, 4000)
    document.addEventListener('pointerdown', onActivity)
    document.addEventListener('keydown', onActivity)
    document.addEventListener('fullscreenchange', () => {
      state.system.fullscreen = Boolean(document.fullscreenElement)
    })
    bump()
  },
  stop() {
    if (!started) return
    started = false
    clearInterval(timer)
    clearInterval(systemTimer)
    document.removeEventListener('pointerdown', onActivity)
    document.removeEventListener('keydown', onActivity)
  },
  getSettings,
  set(playlistId, path, value) {
    updateSettings(playlistId, (settings) => setPath(settings, path, value))
  },
  timerControl(playlistId, action) {
    updateSettings(playlistId, (settings) => {
      const t = settings.modeSettings.timer
      if (action === 'pause' && t.running) {
        t.accumulatedMs += state.now - t.startedAt
        t.running = false
        t.pausedAt = state.now
      } else if (action === 'resume' && !t.running) {
        t.startedAt = state.now
        t.pausedAt = null
        t.running = true
      } else if (action === 'reset') {
        t.accumulatedMs = 0
        t.startedAt = state.now
        t.pausedAt = null
        t.running = true
      }
    })
  },
  timerRemainingMs,
  timerTotalMs,
  nextChangeTimestamp(playlistId) {
    const settings = getSettings(playlistId)
    if (!settings) return null
    if (settings.mode !== 'Timer') return null
    return state.now + timerRemainingMs(settings)
  },
  setSystem(patch) {
    Object.assign(state.system, patch)
    bump()
  },
  setWeather(weather) {
    state.weather = weather
    bump()
  },
  setTemperature(temperature) {
    state.temperature = temperature
    bump()
  },
  setSunTimes(sunrise, sunset) {
    state.sunTimes = { sunrise, sunset }
    bump()
  },
  setCustomRuleSource(playlistId, source) {
    this.set(playlistId, ['modeSettings', 'customRuleSource'], source)
  },
  transitionName,
  fireChange(playlistId, reason) {
    const settings = getSettings(playlistId)
    if (settings) fireChange(settings, reason ?? 'Manual')
  },
  presets: {
    save(name) {
      const id = `preset-${Date.now()}`
      const settings = state.currentPlaylistId ? getSettings(state.currentPlaylistId) : null
      if (!settings) return null
      const snapshot = structuredClone(settings)
      snapshot.modeSettings.timer.startedAt = Date.now()
      snapshot.modeSettings.timer.accumulatedMs = 0
      presets.push({ id, name, settings: snapshot, createdAt: Date.now() })
      bump()
      return id
    },
    load(id) {
      const preset = presets.find((p) => p.id === id)
      if (!preset || !state.currentPlaylistId) return
      const settings = getSettings(state.currentPlaylistId)
      const loaded = structuredClone(preset.settings)
      loaded.modeSettings.timer.startedAt = Date.now()
      loaded.lastChangeAt = Date.now()
      Object.assign(settings, loaded)
      bump()
    },
    reset(id) {
      presets = presets.map((p) => {
        if (p.id !== id) return p
        const fresh = deepDefault()
        return { ...p, settings: fresh, createdAt: Date.now() }
      })
      bump()
    },
    duplicate(id) {
      const preset = presets.find((p) => p.id === id)
      if (!preset) return
      presets.push({ ...structuredClone(preset), id: `preset-${Date.now()}`, name: `${preset.name} Copy`, createdAt: Date.now() })
      bump()
    },
    delete(id) {
      presets = presets.filter((p) => p.id !== id)
      bump()
    },
    apply(id) {
      this.load(id)
    },
    export(id) {
      const preset = presets.find((p) => p.id === id)
      if (!preset) return
      downloadJson(`rex-preset-${preset.name}.json`, JSON.stringify({ type: 'rex-preset', version: 1, preset }, null, 2))
    },
    import(text) {
      const parsed = JSON.parse(text)
      const preset = parsed?.preset ?? parsed
      if (!preset?.name || !preset?.settings) throw new Error('Invalid preset file')
      presets.push({ ...structuredClone(preset), id: `preset-${Date.now()}`, createdAt: Date.now() })
      bump()
      return true
    },
  },
  manage: {
    saveDefaults(playlistId) {
      const settings = getSettings(playlistId)
      if (!settings) return
      const copy = structuredClone(settings)
      copy.modeSettings.timer.startedAt = Date.now()
      copy.modeSettings.timer.accumulatedMs = 0
      defaultTemplate = copy
      bump()
    },
    resetSettings(playlistId) {
      if (!playlistId) return
      settingsByPlaylist[playlistId] = defaultTemplate ? mergeDeep(deepDefault(), defaultTemplate) : deepDefault()
      bump()
    },
    createPlaylist(name, icon, sourceId) {
      const id = createPlaylist(name, icon)
      if (sourceId && settingsByPlaylist[sourceId]) {
        settingsByPlaylist[id] = structuredClone(settingsByPlaylist[sourceId])
      } else {
        settingsByPlaylist[id] = defaultTemplate ? mergeDeep(deepDefault(), defaultTemplate) : deepDefault()
      }
      bump()
      return id
    },
    duplicatePlaylist(id) {
      const snapshot = getSnapshot()
      const source = snapshot.playlists.find((p) => p.id === id)
      const newId = duplicateStorePlaylist(id)
      if (source && settingsByPlaylist[id] && newId) {
        settingsByPlaylist[newId] = structuredClone(settingsByPlaylist[id])
      }
      bump()
      return newId
    },
    renamePlaylist(id, name) {
      renameStorePlaylist(id, name)
    },
    deletePlaylist(id) {
      deleteStorePlaylist(id)
      delete settingsByPlaylist[id]
      bump()
    },
    exportPlaylist(id) {
      const snapshot = getSnapshot()
      const playlist = snapshot.playlists.find((p) => p.id === id)
      if (!playlist) return
      const settings = settingsByPlaylist[id] ?? deepDefault()
      downloadJson(`rex-playlist-${playlist.name}.json`, JSON.stringify({ type: 'rex-playlist', version: 1, playlist: { name: playlist.name, icon: playlist.icon }, settings }, null, 2))
    },
    importPlaylist(text) {
      const parsed = JSON.parse(text)
      const data = parsed?.playlist ? parsed : { playlist: parsed, settings: parsed?.settings }
      const id = createPlaylist(data.playlist?.name ?? 'Imported', data.playlist?.icon ?? 'music')
      if (data.settings) settingsByPlaylist[id] = data.settings
      bump()
      return id
    },
    backup() {
      const snapshot = getSnapshot()
      downloadJson('rex-playlist-backup.json', JSON.stringify({
        type: 'rex-playlist-backup',
        version: 1,
        playlists: snapshot.playlists,
        settingsByPlaylist,
        presets,
        history,
      }, null, 2))
    },
    restore(text) {
      const parsed = JSON.parse(text)
      if (!parsed.playlists) throw new Error('Invalid backup file')
      restorePlaylists(parsed.playlists)
      if (parsed.settingsByPlaylist) settingsByPlaylist = parsed.settingsByPlaylist
      if (Array.isArray(parsed.presets)) presets = parsed.presets
      if (parsed.history) history = parsed.history
      bump()
      return true
    },
  },
  history: {
    clear() {
      history = { recent: [], mostPlayed: {}, skipped: [], failed: [], playbackLog: [], transitionLog: [] }
      bump()
    },
    skip(id) {
      history.skipped.unshift({ t: Date.now(), id })
      history.skipped = history.skipped.slice(0, 100)
      bump()
    },
  },
  validate: validatePlaylist,
}

function onActivity() {
  state.idleSince = Date.now()
}

function downloadJson(filename, content) {
  const blob = new Blob([content], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = filename
  a.click()
  URL.revokeObjectURL(url)
}

export function usePlaylistEngine() {
  const snapshot = useSyncExternalStore(
    (listener) => {
      listeners.add(listener)
      return () => listeners.delete(listener)
    },
    () => {
      if (!hookSnapshot) hookSnapshot = { settingsByPlaylist, presets, history, now: state.now, system: state.system, weather: state.weather }
      return hookSnapshot
    },
  )
  return { engine: playlistEngine, ...snapshot }
}
