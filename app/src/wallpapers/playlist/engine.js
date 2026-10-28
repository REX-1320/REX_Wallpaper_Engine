import { useSyncExternalStore } from 'react'
import { detectFileType } from '../registry'
import { adoptAppliedPath, applyWallpaper, arePlaylistsHydrated, createPlaylist, deletePlaylist as deleteStorePlaylist, ensureWallpaperForPath, getSnapshot, renamePlaylist as renameStorePlaylist, restorePlaylists, subscribeWallpaperApplied, updateWallpaperProperty, duplicatePlaylist as duplicateStorePlaylist } from '../store'
import { ENGINE_MODE_BY_MODE } from '../desktop'
import { appliedWallpaper, memberKey, resolveDesktopPlaylist, wallpaperPath } from './scope'

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

// Timer and scene-end are scheduled by MAIN (electron/playlist-scheduler.cjs):
// they have to keep firing when this window is destroyed to the tray, and at a
// --background login start where no renderer exists. The engine mirrors the
// rotation plan instead of evaluating these modes here - running both would
// rotate twice. Every other mode still runs in the renderer because it uses
// signals only the UI has (idle, lock screen, simulated system state...).
const MAIN_SCHEDULED_MODES = new Set(['Timer', 'After Current Ends', 'Video Ends', 'Audio Ends'])

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

const STORAGE_KEY = 'paiint.playlistEngine.v1'
// Pre-rename key. Read once so presets/history survive the rename, then removed.
const LEGACY_STORAGE_KEY = 'rex.playlistEngine.v1'

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
  // The file that is on the desktop right now (the runtime's last-applied
  // path). The store learns it as appliedId, but only once it has loaded the
  // file - at boot the wallpaper is already playing before the store knows
  // about it, and the playlist that holds it is found by path.
  lastAppliedPath: null,
}

let version = 0
const listeners = new Set()
let hookSnapshot = null
let timer = null
let systemTimer = null
let started = false

let persistTimer = null
// Main replays the wallpaper while the renderer is still mounting, so the
// first getLastApplied() can come back empty (the same race store.js handles).
// The desktop's path is what names the governing playlist, so the engine has
// to keep asking until it is known - otherwise no mode (timer, scene end...)
// ever runs on a cold start.
let lastAppliedRetryAt = 0
let lastAppliedPending = false
function schedulePersist() {
  // Debounced disk write: many bumps (ticks, drags) collapse into one
  // localStorage write every 2s instead of every 500ms tick.
  if (persistTimer) return
  persistTimer = setTimeout(() => {
    persistTimer = null
    persist()
  }, 2000)
}
function flushPersist() {
  if (persistTimer) {
    clearTimeout(persistTimer)
    persistTimer = null
  }
  persist()
}

function bump() {
  version += 1
  hookSnapshot = null
  for (const listener of listeners) listener()
  schedulePersist()
  // Any settings/membership/current-file change can change the rotation plan
  // main is running. Debounced: dragging a slider fires a burst of bumps.
  schedulePlanSync()
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
    let raw = localStorage.getItem(STORAGE_KEY)
    if (!raw) {
      const legacy = localStorage.getItem(LEGACY_STORAGE_KEY)
      if (legacy) {
        localStorage.setItem(STORAGE_KEY, legacy)
        localStorage.removeItem(LEGACY_STORAGE_KEY)
        raw = legacy
      }
    }
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

// The playlist that governs the wallpaper on the desktop.
//
// The store's applied record is authoritative; the path the runtime reported
// at boot is only a fallback until that record exists. A gallery file that is
// not a playlist member therefore stops the rotation instead of inheriting
// the previous playlist's rules (see scope.js).
function activePlaylist() {
  return resolveDesktopPlaylist(getSnapshot(), state.lastAppliedPath)
}

// The wallpaper the engine rotates FROM: the one on the desktop, else the
// selected one (a preview).
function currentWallpaper(snapshot) {
  return appliedWallpaper(snapshot) ?? snapshot.wallpapers.find((w) => w.id === snapshot.selectedId) ?? null
}

function activePlaylistId() {
  return activePlaylist()?.id ?? null
}

// The engine rotates through the playlist's FILES. The store only knows the
// files the user has touched in this session (the gallery restores its tiles
// lazily), so at boot the playlist's pool was empty and the engine could
// never change anything. Materialise the members once per playlist: the
// store adopts them, and the file that is on the desktop becomes the
// store's applied wallpaper (syncAppliedId), which is what scopes the
// scene-end signal.
let preparedPoolKey = null
let preparedPoolReady = false
function preparePlaylistPool(playlist) {
  if (!playlist) return
  const members = playlist.members ?? []
  const key = `${playlist.id}:${members.map(memberKey).join('>')}`
  if (preparedPoolKey === key) return
  preparedPoolKey = key
  preparedPoolReady = members.length === 0
  if (members.length === 0) {
    schedulePlanSync(0)
    return
  }
  const loads = members.map((member) =>
    Promise.resolve(ensureWallpaperForPath(member, { keepSelection: true })).catch(() => null),
  )
  Promise.all(loads).then(() => {
    if (preparedPoolKey !== key) return // a newer membership superseded this pass
    preparedPoolReady = true
    // The queue needs the members' settings (fit mode, volume, ...). Sync the
    // plan only after they are in the store.
    schedulePlanSync(0)
  })
}

function filterPool(settings, pool, members) {
  const { filters, rules } = settings
  // A playlist's members ARE its pool — the dock's strip and the engine must
  // agree on what the playlist contains. A playlist with no members keeps the
  // old behaviour (every wallpaper that passes the filters), so an empty
  // playlist never breaks rotation.
  const memberKeys = Array.isArray(members) ? new Set(members.map(memberKey).filter(Boolean)) : null
  const source = memberKeys && memberKeys.size > 0
    ? pool.filter((w) => memberKeys.has(memberKey(wallpaperPath(w))))
    : pool
  const filtered = source.filter((w) => {
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
    if (filters.tags) {
      const rawTags = w.info?.tags
      const tagList = Array.isArray(rawTags)
        ? rawTags
        : String(rawTags ?? '').split(',').map((s) => s.trim().toLowerCase()).filter(Boolean)
      if (!tagList.some((t) => t.includes(filters.tags.toLowerCase()))) return false
    }
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
  // The playlist's OWN order decides the rotation. The store's insertion
  // order is the order the files were loaded in (the engine materialises
  // them in parallel), not the order the user put them in the playlist.
  if (memberKeys && memberKeys.size > 0) {
    const order = new Map((members ?? []).map((m, index) => [memberKey(m), index]))
    filtered.sort((a, b) => (order.get(memberKey(wallpaperPath(a))) ?? Number.MAX_SAFE_INTEGER) - (order.get(memberKey(wallpaperPath(b))) ?? Number.MAX_SAFE_INTEGER))
  }
  return filtered
}

function pickNext(settings, pool, currentId) {
  if (pool.length === 0) return null
  if (settings.order === 'Repeat One' || settings.options.loopCurrent) {
    return pool.find((w) => w.id === currentId) ?? pool[0]
  }
  // "One run" (options.loopPlaylist off): every file plays once and then the
  // playlist stops - there is no next file, so nothing changes any more and
  // the wallpaper stays on the last one. The played set lives in the
  // settings (like the shuffle order), so it is per playlist and survives
  // the tick. options.loopPlaylist used to be read NOWHERE: the toggle was
  // dead, and a playlist could only ever loop.
  if (!settings.options.loopPlaylist) {
    const played = settings._playedOnce ?? (settings._playedOnce = [])
    if (currentId && !played.includes(currentId)) played.push(currentId)
    const remaining = pool.filter((w) => !played.includes(w.id))
    if (remaining.length === 0) return null
    // The remaining files keep the playlist's order, so "Sequential" still
    // plays them in order; the configured order logic picks among them.
    return pickNextInner(settings, remaining, currentId)
  }
  return pickNextInner(settings, pool, currentId)
}

function pickNextInner(settings, pool, currentId) {
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

// ─── Rotation plan mirrored to main ────────────────────────────────────────
//
// The renderer owns the *selection* logic (membership, filters, order) and
// the per-file settings; main owns the *clock*. Sending an ordered queue of
// fully-described members means main needs no access to localStorage, and a
// rotation it performs is byte-for-byte the same apply the UI would have done.
//
// The queue order is cached per (playlist order + pool) so Random/Shuffle do
// not reshuffle on every 500 ms tick. A real membership/order change starts a
// new cycle, which is the expected behaviour.

let planOrderKey = null
let planOrderIds = null
let lastPlanSignature = null
let planSyncPending = false
let planSyncQueued = false
let planSyncTimer = null
let schedulerStatePending = false
let unsubscribePlaylistApplied = null
let unsubscribeWallpaperApplied = null
// While a main-process rotation is being adopted, the store still describes
// the PREVIOUS file. Building a plan in that window would mirror the old path
// (and an already-expired timer) back to main, which would rotate a second
// time. No plan is sent until adoption (and the playlist tag) is done.
let schedulerApplyPending = 0
// Every plan/stop send takes a sequence number: an older in-flight invoke must
// not mark its signature as current after a newer stop/plan has been sent.
let planSendSeq = 0

function playlistPoolKey(playlist) {
  if (!playlist) return null
  return `${playlist.id}:${(playlist.members ?? []).map(memberKey).join('>')}`
}

function weightedOrder(pool) {
  const remaining = [...pool]
  const ordered = []
  while (remaining.length > 0) {
    const weights = remaining.map((w) => {
      let weight = 1
      if (w.general?.favorite) weight += 3
      if (w.info?.dateAdded) weight += 1
      return weight
    })
    const total = weights.reduce((a, b) => a + b, 0)
    let roll = Math.random() * total
    let index = remaining.length - 1
    for (let i = 0; i < remaining.length; i += 1) {
      roll -= weights[i]
      if (roll <= 0) {
        index = i
        break
      }
    }
    ordered.push(remaining[index].id)
    remaining.splice(index, 1)
  }
  return ordered
}

function orderPool(settings, pool, currentId) {
  if (pool.length === 0) return []
  if (settings.order === 'Repeat One' || settings.options.loopCurrent) {
    const current = pool.find((w) => w.id === currentId) ?? pool[0]
    return [current]
  }
  const key = [
    settings.order,
    settings.options.loopPlaylist ? 1 : 0,
    (settings.modeSettings.customOrder ?? []).join(','),
    pool.map((w) => `${w.id}:${memberKey(wallpaperPath(w))}`).join('>'),
  ].join('|')
  if (key !== planOrderKey) {
    planOrderKey = key
    const ids = pool.map((w) => w.id)
    switch (settings.order) {
      case 'Random':
      case 'Shuffle':
        planOrderIds = shuffleArray(ids)
        break
      case 'Weighted Random':
        planOrderIds = weightedOrder(pool)
        break
      case 'Recently Added':
      case 'Newest First':
        planOrderIds = sortByDate(pool, true).map((w) => w.id)
        break
      case 'Oldest First':
        planOrderIds = sortByDate(pool, false).map((w) => w.id)
        break
      case 'Alphabetical':
        planOrderIds = sortByName(pool, true).map((w) => w.id)
        break
      case 'Custom Order': {
        const configured = (settings.modeSettings.customOrder ?? []).filter((id) => pool.some((w) => w.id === id))
        planOrderIds = [...configured, ...pool.filter((w) => !configured.includes(w.id)).map((w) => w.id)]
        break
      }
      case 'Favorites First':
        planOrderIds = [
          ...pool.filter((w) => w.general?.favorite).map((w) => w.id),
          ...pool.filter((w) => !w.general?.favorite).map((w) => w.id),
        ]
        break
      case 'Highest Rated':
        planOrderIds = [...pool].sort((a, b) => (b.general?.rating ?? 0) - (a.general?.rating ?? 0)).map((w) => w.id)
        break
      case 'Least Played':
        planOrderIds = [...pool].sort((a, b) => (history.mostPlayed[a.id] ?? 0) - (history.mostPlayed[b.id] ?? 0)).map((w) => w.id)
        break
      case 'Most Played':
        planOrderIds = [...pool].sort((a, b) => (history.mostPlayed[b.id] ?? 0) - (history.mostPlayed[a.id] ?? 0)).map((w) => w.id)
        break
      case 'Sequential':
      case 'Repeat All':
      default:
        planOrderIds = ids
    }
  }
  const byId = new Map(pool.map((w) => [w.id, w]))
  const ordered = (planOrderIds ?? []).map((id) => byId.get(id)).filter(Boolean)
  // Defensive: never drop a member the user just added because the cached
  // order predates it (the key above should already prevent this).
  if (ordered.length < pool.length) {
    for (const wallpaper of pool) if (!ordered.includes(wallpaper)) ordered.push(wallpaper)
  }
  return ordered
}

function memberApplyPayload(w) {
  if (!w) return null
  const type = detectFileType(w.info?.source ?? wallpaperPath(w) ?? '') ?? 'IMAGE'
  if (type === 'IMAGE') {
    return { kind: 'image', mode: ENGINE_MODE_BY_MODE[w.display?.mode] ?? 'fill' }
  }
  if (type !== 'VIDEO') return null // WEB/SCENE/SHADER/AUDIO are not runtime scenes
  return {
    kind: 'video',
    mode: ENGINE_MODE_BY_MODE[w.display?.mode] ?? 'fill',
    loop: w.playback?.loop ?? true,
    muted: w.playback?.mute ?? false,
    volume: (w.playback?.volume ?? 100) / 100,
    speed: w.playback?.speed ?? 1,
    audioMode: w.playback?.audioMode,
    // The engine's colour scale is 0..2 with 1.0 = unchanged; the app keeps
    // percent 0..200 where 100 = unchanged.
    brightness: (w.appearance?.brightness ?? 100) / 100,
    contrast: (w.appearance?.contrast ?? 100) / 100,
    saturation: (w.appearance?.saturation ?? 100) / 100,
  }
}

// undefined = "not ready yet, keep the plan main already has"
// null      = "the desktop file is not governed by a playlist, stop the plan"
function buildRotationPlan() {
  const snapshot = getSnapshot()
  const current = appliedWallpaper(snapshot)
  // The runtime path is updated synchronously by every apply (and by main's
  // applied event); the store record can lag behind an async adopt. Prefer the
  // path so a plan built in that window cannot describe the previous file.
  const currentPath = state.lastAppliedPath ?? wallpaperPath(current)
  if (!currentPath) return undefined
  if (!arePlaylistsHydrated()) return undefined
  const active = resolveDesktopPlaylist(snapshot, state.lastAppliedPath)
  // NO_PLAYLIST (an explicit 'None' pick) is a stop, not a playlist.
  if (!active?.id) return null
  // Materialise this playlist's members (they are what the queue rotates
  // through). Preparation is keyed by the member list, so this is a no-op
  // once the store has them.
  preparePlaylistPool(active)
  const members = active.members ?? []
  if (members.length > 0 && (!preparedPoolReady || preparedPoolKey !== playlistPoolKey(active))) {
    // The members exist but their records/settings are still loading. Do not
    // overwrite the persisted plan with an empty one in the meantime.
    return undefined
  }
  const settings = getSettings(active.id)
  if (!settings) return null
  const pool = filterPool(settings, poolWallpapers(), members)
  let anchorId = current?.id ?? null
  if (!anchorId || !pool.some((w) => w.id === anchorId)) {
    const key = memberKey(currentPath)
    anchorId = pool.find((w) => memberKey(wallpaperPath(w)) === key)?.id ?? null
  }
  const ordered = orderPool(settings, pool, anchorId)
  const queue = ordered
    .map((w) => {
      const path = wallpaperPath(w)
      const apply = memberApplyPayload(w)
      return path && apply ? { path, apply } : null
    })
    .filter(Boolean)
  const timer = settings.modeSettings.timer
  return {
    version: 1,
    playlistId: active.id,
    playlistName: active.name,
    mode: settings.mode,
    currentPath,
    loop: settings.options.loopPlaylist !== false,
    paused: Boolean(settings.options.pause),
    updatedAt: Date.now(),
    queue,
    timer: {
      totalMs: timerTotalMs(settings),
      remainingMs: timerRemainingMs(settings),
      running: Boolean(timer.running),
      startedAt: timer.startedAt,
      accumulatedMs: timer.accumulatedMs,
      syncedAt: Date.now(),
    },
  }
}

function planSignature(plan) {
  if (!plan) return 'null'
  return JSON.stringify({
    playlistId: plan.playlistId,
    playlistName: plan.playlistName,
    mode: plan.mode,
    currentPath: plan.currentPath,
    loop: plan.loop,
    paused: plan.paused,
    queue: plan.queue,
    // remainingMs/syncedAt tick every second and are NOT changes; the stable
    // timer identity (start/reset/pause) is what main re-arms on.
    timer: {
      totalMs: plan.timer.totalMs,
      running: plan.timer.running,
      startedAt: plan.timer.startedAt,
      accumulatedMs: plan.timer.accumulatedMs,
    },
  })
}

function schedulePlanSync(delay = 150) {
  if (!started) return
  if (planSyncPending) {
    planSyncQueued = true
    return
  }
  if (planSyncTimer) clearTimeout(planSyncTimer)
  planSyncTimer = setTimeout(() => {
    planSyncTimer = null
    syncPlan()
  }, delay)
}

function syncPlan() {
  if (!started || planSyncPending || schedulerStatePending || schedulerApplyPending > 0) return
  const bridge = window.paiintDesktop?.syncPlaylistPlan
  if (typeof bridge !== 'function') return
  const plan = buildRotationPlan()
  if (plan === undefined) return
  const signature = planSignature(plan)
  if (signature === lastPlanSignature) return
  planSyncPending = true
  const seq = ++planSendSeq
  Promise.resolve(bridge(plan))
    .then((schedulerState) => {
      // A newer stop/plan was sent while this invoke was in flight; its
      // signature owns `lastPlanSignature`, not this older response.
      if (seq === planSendSeq) lastPlanSignature = signature
      applySchedulerState(schedulerState)
    })
    .catch(() => {
      // Main is not ready / app is quitting. Keep the signature unsent so the
      // next tick retries.
      if (seq === planSendSeq) lastPlanSignature = null
    })
    .finally(() => {
      planSyncPending = false
      if (planSyncQueued) {
        planSyncQueued = false
        schedulePlanSync(0)
      }
    })
}

// Main is authoritative about the live countdown. When the UI reopens after
// rotations happened with no renderer, the persisted timer can be older than
// main's due time; align it once instead of showing a countdown that no longer
// matches when the wallpaper will change. The next plan sync then carries the
// same timer identity, so main keeps (rather than resets) its deadline.
function applySchedulerState(schedulerState) {
  if (!schedulerState?.nextDueAt || !schedulerState.playlistId) return
  const settings = getSettings(schedulerState.playlistId)
  if (!settings || settings.mode !== 'Timer' || !settings.modeSettings.timer.running) return
  const total = timerTotalMs(settings)
  if (total <= 0) return
  const dueIn = schedulerState.nextDueAt - Date.now()
  if (dueIn < -1_500) return
  const localDueIn = timerRemainingMs(settings)
  if (Math.abs(dueIn - localDueIn) < 1_500) return
  const timerState = settings.modeSettings.timer
  timerState.accumulatedMs = 0
  timerState.running = true
  timerState.pausedAt = null
  timerState.startedAt = Date.now() - Math.max(0, total - Math.max(0, dueIn))
  // Re-sync with the corrected timer identity; main keeps the same due time.
  bump()
}

// A user apply is an explicit start: the playlist's interval begins NOW, not
// at the timestamp it happened to carry from a previous session. Without this,
// an overdue Timer fires the member AFTER the one the user just applied as
// soon as main receives the plan ("apply playlist -> it immediately jumps
// again").
function restartPlaylistTimer(name) {
  const playlist = (getSnapshot().playlists ?? []).find((p) => p.name === name)
  if (!playlist) return
  const settings = getSettings(playlist.id)
  const timer = settings.modeSettings.timer
  timer.accumulatedMs = 0
  timer.pausedAt = null
  timer.startedAt = Date.now()
  if (settings.mode === 'Timer') timer.running = true
  settings.lastChangeAt = timer.startedAt
  // Applying a one-run playlist starts a new run.
  settings._playedOnce = []
}

// Stop the rotation main is running RIGHT NOW. An apply must cancel the old
// plan before the debounced plan sync mirrors the new one: otherwise the
// previous playlist's Timer can fire in that window (or while the new
// playlist's members are still loading) and put its own file on the desktop.
function stopMainRotation() {
  if (!started) return
  const bridge = window.paiintDesktop?.syncPlaylistPlan
  if (typeof bridge !== 'function') return
  const seq = ++planSendSeq
  Promise.resolve(bridge(null))
    .then((schedulerState) => {
      if (seq === planSendSeq) lastPlanSignature = planSignature(null)
      applySchedulerState(schedulerState)
    })
    .catch(() => {
      if (seq === planSendSeq) lastPlanSignature = null
    })
}

// The store announced an apply before React re-rendered. Keep the engine's
// notion of the desktop path synchronous, restart the target playlist's timer,
// and cancel the plan of whatever was rotating before.
function handleWallpaperApplied({ path, source, playlist }) {
  state.lastAppliedPath = path ?? null
  if (source === 'playlist' && playlist) restartPlaylistTimer(playlist)
  if (source === 'playlist' || source === 'gallery') stopMainRotation()
  schedulePlanSync(0)
}

// A rotation main performed (Timer or a native scene end). Adopt the file in
// the store without asking DesktopApply to LOAD it again, and reset the local
// countdown so the dialog shows the new interval immediately.
function handlePlaylistApplied(event) {
  if (!event || event.event !== 'applied' || !event.path) return
  state.lastAppliedPath = event.path
  const id = event.playlistId
  if (id && event.mode === 'Timer') {
    const settings = getSettings(id)
    const timerState = settings.modeSettings.timer
    timerState.accumulatedMs = 0
    timerState.startedAt = Number(event.at) || Date.now()
    timerState.running = true
    timerState.pausedAt = null
    settings.lastChangeAt = timerState.startedAt
  }
  schedulerApplyPending += 1
  adoptAppliedPath(event.path, { keepSelection: true, silent: true })
    .then((wallpaperId) => {
      if (!wallpaperId) return
      // A rotation is a playlist apply: the file now belongs to the playlist
      // that is rotating it. Without this tag an old explicit 'None' on the
      // record (from a gallery apply in a previous session) made the engine
      // resolve NO_PLAYLIST right after the first rotation and clear main's
      // plan - the dock then showed "Apply playlist" again.
      if (event.playlistName && event.playlistName !== 'None') {
        updateWallpaperProperty(wallpaperId, ['playlist', 'currentPlaylist'], event.playlistName)
      }
      recordExternalPlay(wallpaperId)
    })
    .catch(() => {})
    .finally(() => {
      schedulerApplyPending = Math.max(0, schedulerApplyPending - 1)
      bump()
      schedulePlanSync(0)
    })
}

function recordExternalPlay(id) {
  const wallpaper = getSnapshot().wallpapers.find((w) => w.id === id)
  if (!wallpaper) return
  history.recent = [id, ...history.recent.filter((x) => x !== id)].slice(0, 50)
  history.mostPlayed[id] = (history.mostPlayed[id] ?? 0) + 1
  history.playbackLog.unshift({
    t: Date.now(),
    id,
    name: wallpaper.info?.name ?? 'Wallpaper',
    type: detectFileType(wallpaper.info?.source ?? '') ?? 'IMAGE',
  })
  history.playbackLog = history.playbackLog.slice(0, 200)
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

// Changes the wallpaper to the next file of the active playlist. This is the
// renderer-only path (Manual, schedules, idle, ...); Timer and scene-end are
// applied by main from the mirrored plan.
//
// Returns true when the wallpaper actually changed. A change that cannot
// happen (no active playlist, nothing left to play) returns false.
function fireChange(settings, _reason) {
  const snapshot = getSnapshot()
  // The rotation runs from the wallpaper that is ON THE DESKTOP (a preview
  // selection is not what is playing), and it is governed by the playlist
  // that holds it.
  const current = currentWallpaper(snapshot)
  const active = resolveDesktopPlaylist(snapshot, state.lastAppliedPath)
  const activeName = active?.name ?? null
  const pool = filterPool(settings, poolWallpapers(), active?.members)
  if (activeName && activeName !== 'None' && active) {
    const changeNow = () => {
      const next = pickNext(settings, pool, current?.id ?? null)
      if (!next) return false
      if (settings.options.validateBeforePlayback) {
        const issues = validatePlaylist(pool)
        if (issues.length > 0) history.failed.push({ t: Date.now(), reason: 'validation', issues: issues.slice(0, 3) })
      }
      const transition = transitionName(settings, next)
      recordPlay(next, transition)
      settings.lastChangeAt = Date.now()
      applyWallpaper(next.id, { source: 'engine' })
      // The wallpaper changed: the path is what the desktop shows from now on.
      state.lastAppliedPath = wallpaperPath(next)
      return true
    }
    return changeNow()
  }
  return false
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
  const elapsed = t.accumulatedMs + (t.running ? Math.max(0, state.now - t.startedAt) : 0)
  return Math.max(0, total - elapsed)
}

function evaluateMode(settings) {
  const ms = settings.modeSettings
  const now = state.now
  const mode = settings.mode
  if (settings.options.pause) return null
  // Timer and scene-end are main's job now: they have to keep firing after
  // this window is destroyed and at a --background login start. Evaluating
  // them here would run a second scheduler and rotate twice.
  if (MAIN_SCHEDULED_MODES.has(mode)) return null
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
  // Fresh clock every tick, but notify subscribers + persist ONLY when
  // something actually changed (previously bumped unconditionally 2x/sec,
  // churning localStorage and re-rendering consumers forever).
  state.now = Date.now()
  // Cold-start race: keep asking main which file is on the desktop until it
  // answers. Without it there is no playlist to evaluate and the timer (or
  // any other mode) silently never fires.
  if (!state.lastAppliedPath && !lastAppliedPending && state.now - lastAppliedRetryAt >= 2000) {
    lastAppliedPending = true
    lastAppliedRetryAt = state.now
    Promise.resolve(window.paiintDesktop?.getLastApplied?.())
      .then((last) => {
        if (last?.path && !state.lastAppliedPath) {
          state.lastAppliedPath = last.path
          adoptAppliedPath(last.path, { keepSelection: true, silent: true }).catch(() => {})
          bump()
        }
      })
      .catch(() => {})
      .finally(() => {
        lastAppliedPending = false
      })
  }
  let changed = false
  const id = activePlaylistId()
  // A gallery apply (or a desktop file outside every playlist) resolves to
  // null. Clear the cached id instead of keeping the previous playlist's
  // settings alive: otherwise a stale Random Interval / schedule kept
  // evaluating against a desktop it no longer governs.
  if (id !== state.currentPlaylistId) {
    state.currentPlaylistId = id
    if (id) {
      const settings = getSettings(id)
      if (settings.startup.randomOnStartup) fireChange(settings, 'Startup')
    }
    changed = true
  }
  // The playlist's files have to be in the store before the engine can
  // rotate through them - at boot the store knows none of them. The plan
  // builder prepares the APPLIED playlist (not a preview selection).
  const settings = state.currentPlaylistId ? getSettings(state.currentPlaylistId) : null
  if (settings) {
    const reason = evaluateMode(settings)
    if (reason) {
      // A reason that cannot change anything yet (no active playlist, the
      // playlist ran out) must not re-render the UI every tick.
      changed = fireChange(settings, reason) || changed
    }
    if (settings.automation.enabled && evaluateAutomation(settings)) {
      const target = (getSnapshot().playlists ?? []).find((p) => p.name === settings.automation.switchTo)
      if (target) {
        const snapshot = getSnapshot()
        const selected = snapshot.wallpapers.find((w) => w.id === snapshot.selectedId)
        if (selected) {
          updateWallpaperProperty(selected.id, ['playlist', 'currentPlaylist'], target.name)
          changed = true
        }
      }
    }
  }
  // Mirror the rotation the user configured into main. The tick is the
  // heartbeat: settings edits, membership changes and the initial pool load
  // all converge here even if an invoke was dropped while main was starting.
  syncPlan()
  if (changed) bump()
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
    // Ask main for the authoritative countdown BEFORE mirroring a plan. The
    // persisted renderer timer can be stale after rotations main performed
    // with no window; sending it first would reset main's deadline.
    const schedulerStateRequest = window.paiintDesktop?.getPlaylistSchedulerState?.()
    if (schedulerStateRequest?.then) {
      schedulerStatePending = true
      Promise.resolve(schedulerStateRequest)
        .then((schedulerState) => applySchedulerState(schedulerState))
        .catch(() => {})
        .finally(() => {
          schedulerStatePending = false
          schedulePlanSync(0)
        })
    }
    // Rotations main performs come back as events so an open window adopts
    // the new file (and its countdown) without re-applying it.
    unsubscribePlaylistApplied = window.paiintDesktop?.onPlaylistApplied?.(handlePlaylistApplied) ?? null
    // Applies are announced by the store before React re-renders: the old
    // rotation has to stop at the click, not one plan-sync later.
    unsubscribeWallpaperApplied = subscribeWallpaperApplied(handleWallpaperApplied)
    // The wallpaper that is on the desktop right now. Main re-applied it at
    // boot before the store knew about it, so the engine asks the runtime
    // which file is playing: that path names the playlist that governs it.
    window.paiintDesktop?.getLastApplied?.()
      ?.then((last) => {
        if (last?.path && !state.lastAppliedPath) {
          state.lastAppliedPath = last.path
          adoptAppliedPath(last.path, { keepSelection: true, silent: true })
            .then(() => bump())
            .catch(() => {})
        }
      })
      ?.catch?.(() => {})
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
    if (typeof unsubscribePlaylistApplied === 'function') {
      try {
        unsubscribePlaylistApplied()
      } catch {}
    }
    unsubscribePlaylistApplied = null
    if (typeof unsubscribeWallpaperApplied === 'function') {
      try {
        unsubscribeWallpaperApplied()
      } catch {}
    }
    unsubscribeWallpaperApplied = null
    schedulerApplyPending = 0
    if (planSyncTimer) {
      clearTimeout(planSyncTimer)
      planSyncTimer = null
    }
    planSyncQueued = false
    planSyncPending = false
    schedulerStatePending = false
    lastPlanSignature = null
    preparedPoolKey = null
    preparedPoolReady = false
    state.lastAppliedPath = null
    clearInterval(timer)
    clearInterval(systemTimer)
    document.removeEventListener('pointerdown', onActivity)
    document.removeEventListener('keydown', onActivity)
    // Do NOT clear main's plan here: this stop is usually the window being
    // destroyed, and the rotation is supposed to continue in the tray.
    flushPersist()
  },
  // Stop the rotation that is running on the desktop and leave the last video
  // frame behind as a still. The applied file keeps its identity (it is still
  // the wallpaper the user chose) but it is explicitly opted out of rotation,
  // so no Timer or scene-end rule can fire again this session.
  async stopRotation() {
    const snapshot = getSnapshot()
    const applied = appliedWallpaper(snapshot)
    const active = resolveDesktopPlaylist(snapshot, state.lastAppliedPath)
    if (active?.id) {
      const settings = getSettings(active.id)
      const timerState = settings?.modeSettings?.timer
      if (timerState) {
        timerState.running = false
        timerState.accumulatedMs = 0
        timerState.pausedAt = null
        timerState.startedAt = null
      }
    }
    if (applied) updateWallpaperProperty(applied.id, ['playlist', 'currentPlaylist'], 'None')
    // The still on the desktop is no longer a playlist member as far as the
    // engine is concerned: drop the boot path so a reload before the store
    // re-adopts cannot resurrect the old rotation from it.
    state.lastAppliedPath = null
    // Clear main's plan BEFORE the runtime tears down, so a Timer that is due
    // during the snapshot capture cannot apply another file over the still.
    lastPlanSignature = planSignature(null)
    try {
      await window.paiintDesktop?.syncPlaylistPlan?.(null)
    } catch {
      // Main did not clear it; let the next plan tick retry instead of
      // treating the failed send as delivered.
      lastPlanSignature = null
    }
    try {
      await window.paiintDesktop?.stopWallpaper?.()
    } catch {
      // No runtime running: the desktop is already still.
    }
    bump()
  },
  getSettings,
  // The playlist the engine evaluates right now: the one named on the
  // wallpaper, or (after a restart / after the engine rotated) the one that
  // HOLDS the file on the desktop. The dialog must edit this one - a
  // name-only lookup there resolves the 'None' sentinel and edits a config
  // the engine never evaluates.
  activePlaylist,
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
      downloadJson(`paiint-preset-${preset.name}.json`, JSON.stringify({ type: 'paiint-preset', version: 1, preset }, null, 2))
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
      downloadJson(`paiint-playlist-${playlist.name}.json`, JSON.stringify({ type: 'paiint-playlist', version: 1, playlist: { name: playlist.name, icon: playlist.icon }, settings }, null, 2))
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
      downloadJson('paiint-playlist-backup.json', JSON.stringify({
        type: 'paiint-playlist-backup',
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
