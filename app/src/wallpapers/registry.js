import {
  AudioWaveform,
  Film,
  Flame,
  Folder,
  Gamepad2,
  Globe,
  Heart,
  Image as ImageIcon,
  Mountain,
  Music,
  Palette,
  Shuffle,
  Slash,
  Sparkles,
  Star,
  Video as VideoIcon,
  Zap,
} from 'lucide-react'

export const WALLPAPER_TYPES = ['IMAGE', 'VIDEO', 'WEB', 'SCENE', 'SHADER', 'AUDIO_VISUALIZER']

// v1 reality: only these two reach the engine. Everything else is UI-only
// and must be hidden/badged, never applied. See docs/superpowers/plans/2026-10-08-ui-honesty-supported-types.md
export const SUPPORTED_WALLPAPER_TYPES = ['IMAGE', 'VIDEO']
export const DEFERRED_TYPES = ['WEB', 'SCENE', 'SHADER', 'AUDIO_VISUALIZER']

export function isSupportedWallpaperType(type) {
  return SUPPORTED_WALLPAPER_TYPES.includes(type)
}

export const SUPPORTED_EXTENSIONS = {
  IMAGE: ['.png', '.jpg', '.jpeg', '.bmp', '.tif', '.tiff'],
  VIDEO: ['.mp4', '.mkv', '.avi', '.mov', '.wmv', '.m4v', '.webm', '.mpg', '.mpeg'],
}

export function isSupportedWallpaperPath(path) {
  const ext = path.slice(path.lastIndexOf('.')).toLowerCase()
  return Object.values(SUPPORTED_EXTENSIONS).flat().includes(ext)
}

export const TYPE_META = {
  IMAGE: { label: 'Image', icon: ImageIcon, gradient: 'linear-gradient(135deg,#7c5cff,#ff6ad5)' },
  VIDEO: { label: 'Video', icon: VideoIcon, gradient: 'linear-gradient(135deg,#00c2ff,#6ee7b7)' },
  WEB: { label: 'Web', icon: Globe, gradient: 'linear-gradient(135deg,#38bdf8,#6366f1)' },
  SCENE: { label: 'Scene', icon: Palette, gradient: 'linear-gradient(135deg,#a855f7,#ec4899)' },
  SHADER: { label: 'Shader', icon: Sparkles, gradient: 'linear-gradient(135deg,#f59e0b,#ef4444)' },
  AUDIO_VISUALIZER: {
    label: 'Audio Visualizer',
    icon: AudioWaveform,
    gradient: 'linear-gradient(135deg,#f43f5e,#fb923c)',
  },
}

export const WALLPAPER_MODES = [
  'Cover',
  'Contain',
  'Stretch',
  'Fill',
  'Center',
  'Tile',
  'Span',
  'Fit Width',
  'Fit Height',
]

export const ALIGNMENTS = [
  'Top Left',
  'Top',
  'Top Right',
  'Center Left',
  'Center',
  'Center Right',
  'Bottom Left',
  'Bottom',
  'Bottom Right',
]

export const RENDERER_OPTIONS = ['Auto', 'GPU', 'CPU']
export const FRAME_RATE_LIMITS = ['Unlimited', '120', '90', '60', '30', '15']
export const RENDER_RESOLUTIONS = ['Auto', '100%', '75%', '50%', '25%']
export const POWER_MODES = ['Maximum Quality', 'Balanced', 'Battery Saver']
export const MONITOR_OPTIONS = ['Monitor 1', 'Monitor 2']
export const ACTIVE_TIMES = ['Always', 'Custom Hours', 'Sunrise to Sunset', 'Night Only', 'Day Only']
export const DAY_SCHEDULES = ['Every Day', 'Weekdays', 'Weekends', 'Custom']
export const PLAYLIST_ORDERS = ['Manual', 'Sequential', 'Random', 'Weighted Random']
export const PLAYLISTS = ['None', 'Favorites', 'Nature', 'Gaming', 'Mixed']
export const PLAYLIST_ICON_CHOICES = [
  'music',
  'heart',
  'mountain',
  'gamepad',
  'star',
  'sparkles',
  'folder',
  'film',
  'palette',
  'zap',
  'flame',
  'shuffle',
]
export const PLAYLIST_ICONS = {
  music: Music,
  heart: Heart,
  mountain: Mountain,
  gamepad: Gamepad2,
  star: Star,
  sparkles: Sparkles,
  folder: Folder,
  film: Film,
  palette: Palette,
  zap: Zap,
  flame: Flame,
  shuffle: Shuffle,
  slash: Slash,
}
export const PLAYLIST_ICON_DEFAULTS = ['slash', 'heart', 'mountain', 'gamepad', 'shuffle']
export const TRANSITIONS = ['None', 'Fade', 'Crossfade', 'Slide', 'Zoom', 'Blur', 'Spiral']
export const CODECS = ['Auto', 'H.264 (AVC)', 'H.265 (HEVC)', 'VP9', 'AV1']
export const BROWSER_ENGINES = ['WebView2 (Chromium)', 'WebKit', 'Gecko']
export const CAMERAS = ['Orbit', 'First Person', 'Top Down', 'Isometric']
export const QUALITIES = ['Low', 'Medium', 'High', 'Ultra']
export const LIGHTINGS = ['None', 'Flat', 'PBR', 'Ray Traced']
export const OUTPUT_DEVICES = ['Default', 'Primary', 'Headphones', 'Speakers', 'Virtual Cable']
export const VISUALIZER_MODES = ['Bars', 'Wave', 'Spectrum', 'Circular', 'Hybrid']
export const AUDIO_SOURCES = ['System', 'Microphone', 'Spotify', 'Custom']
export const FFT_SIZES = ['64', '128', '256', '512', '1024', '2048']
export const ENV_OPTIONS = ['Production', 'Development', 'Test']

export function parseDuration(text) {
  const parts = text.split(':').map(Number)
  if (parts.length < 2 || parts.some((n) => Number.isNaN(n))) return 60
  const [m, s, tenths = 0] = parts.length === 2 ? [parts[0], parts[1], 0] : parts
  return m * 60 + s + tenths / 10
}

export function detectFileType(name) {
  const ext = name.split('.').pop().toLowerCase()
  if (['png', 'jpg', 'jpeg', 'gif', 'webp', 'svg', 'bmp', 'ico', 'avif'].includes(ext)) return 'IMAGE'
  if (['mp4', 'webm', 'mkv', 'mov', 'avi', 'm4v', 'mpg'].includes(ext)) return 'VIDEO'
  if (['mp3', 'wav', 'flac', 'ogg', 'aac', 'm4a'].includes(ext)) return 'AUDIO_VISUALIZER'
  if (['html', 'htm', 'css', 'js', 'mjs', 'jsx', 'ts', 'tsx', 'json'].includes(ext)) return 'WEB'
  if (['glsl', 'hlsl', 'frag', 'vert', 'comp', 'shader'].includes(ext)) return 'SHADER'
  if (['glb', 'gltf', 'blend', 'fbx', 'obj', 'stl'].includes(ext)) return 'SCENE'
  return null
}

const DEFAULT_GENERAL = {
  enabled: true,
  favorite: true,
  visible: true,
  startAutomatically: true,
  loadOnStartup: true,
  pauseWhenHidden: true,
  autoResume: true,
  pinWallpaper: false,
  lockEditing: false,
}

const DEFAULT_DISPLAY = {
  monitor: 'Monitor 1',
  mode: 'Cover',
  alignment: 'Center',
  offsetX: 0,
  offsetY: 0,
  scale: 100,
  rotation: 0,
  flipH: false,
  flipV: false,
  crop: 0,
  backgroundColor: '#000000',
  letterboxColor: '#000000',
}

const DEFAULT_PLAYBACK = {
  playing: true,
  paused: false,
  loop: true,
  mute: false,
  volume: 100,
  speed: 1,
  startPosition: 0,
  randomStart: false,
  resumePrevious: true,
  seekTimeline: true,
  autoplay: true,
  normalizeVolume: false,
}

const DEFAULT_APPEARANCE = {
  opacity: 100,
  brightness: 100,
  contrast: 100,
  gamma: 1,
  saturation: 100,
  hue: 0,
  temperature: 50,
  tint: '#ffffff',
  vibrance: 100,
  exposure: 100,
  sharpen: 0,
  blur: 0,
  vignette: 0,
  grayscale: 0,
  invert: 0,
  colorOverlay: '#000000',
  noise: 0,
}

const DEFAULT_PERFORMANCE = {
  renderer: 'GPU',
  frameRateLimit: 'Unlimited',
  renderResolution: 'Auto',
  powerMode: 'Balanced',
  pauseOnBattery: true,
  pauseFullscreen: true,
  pauseGames: true,
  pauseRemoteDesktop: false,
  pauseVideoCalls: false,
}

const DEFAULT_MONITOR = {
  applyTo: 'All',
  differentWallpaper: false,
  spanAcross: false,
  mirror: false,
  independentPlayback: true,
  independentAudio: false,
  independentEffects: false,
}

const DEFAULT_AUDIO = {
  enabled: true,
  volume: 100,
  mute: false,
  outputDevice: 'Default',
  audioReactive: true,
  bassSensitivity: 50,
  trebleSensitivity: 50,
  visualizerMode: 'Bars',
}

const DEFAULT_INTERACTION = {
  mouseInteraction: true,
  clickThrough: false,
  followCursor: false,
  cursorGlow: true,
  cursorRipple: false,
  particles: true,
  touchSupport: true,
  gestureSupport: false,
}

const DEFAULT_SCHEDULE = {
  activeTime: 'Always',
  startTime: 0,
  endTime: 24,
  daySchedule: 'Every Day',
  gamingProfile: false,
  batteryProfile: true,
  sleepProfile: true,
}

const DEFAULT_PLAYLIST = {
  currentPlaylist: 'None',
  order: 'Sequential',
  shuffle: false,
  repeat: true,
  transition: 'Crossfade',
  transitionDuration: 2,
  autoChange: false,
  changeEvery: 15,
  idleTrigger: false,
  batteryTrigger: true,
  weatherTrigger: false,
}

const DEFAULT_ADVANCED = {
  developerMode: false,
  environmentVariables: false,
  logs: true,
  diagnostics: false,
  rendererStats: true,
  metadata: '{"engine":"REX","format":2,"fps":60}',
  variables: 'WALLPAPER_NAME="Aurora Loop"',
  arguments: '--fullscreen --vsync',
}

const RENDERER_DEFAULTS = {
  IMAGE: { zoom: 100, pan: 0, kenBurns: true, slideshow: false, transition: 'Fade' },
  VIDEO: {
    codec: 'H.264 (AVC)',
    hardwareDecode: true,
    frameSkip: true,
    loop: true,
    startPosition: 0,
    subtitle: '',
  },
  WEB: {
    url: 'https://rex.studio/wallpapers/aurora',
    reload: false,
    devTools: false,
    zoom: 100,
    cache: true,
    javascript: true,
    browserEngine: 'WebView2 (Chromium)',
  },
  SCENE: {
    camera: 'Orbit',
    lighting: 'PBR',
    quality: 'High',
    postProcessing: true,
    particles: true,
    physics: false,
  },
  SHADER: {
    shaderFile: 'aurora.frag',
    uniforms: 'uniform float u_time; uniform vec2 u_res;',
    fps: 60,
    textureInputs: 'u_texture0, u_noise',
    canvasSize: '1920 × 1080',
  },
  AUDIO_VISUALIZER: {
    audioSource: 'System',
    fftSize: '1024',
    bars: true,
    wave: false,
    spectrum: true,
    sensitivity: 50,
    peakHold: true,
    customColors: true,
  },
}

export function createWallpaperFromType(type) {
  const meta = TYPE_META[type]
  if (!meta) return null
  return {
    id: `${type.toLowerCase()}-1`,
    type,
    info: {
      name: `Aurora ${meta.label}`,
      author: 'REX Studio',
      source: `C:\\REX\\wallpapers\\aurora`,
      version: '1.2.0',
      license: 'MIT',
      category: 'Nature',
      tags: 'aurora,night,loop',
      description: `Live ${meta.label.toLowerCase()} wallpaper with real-time adjustments.`,
      resolution: type === 'IMAGE' ? '3840 × 2160' : type === 'WEB' ? 'N/A' : '1920 × 1080',
      aspectRatio: type === 'WEB' ? 'N/A' : '16:9',
      duration: type === 'IMAGE' ? '—' : '0:24.0',
      fps: type === 'WEB' ? '60 (WebView)' : '60',
      codec: type === 'VIDEO' ? 'H.264 (AVC)' : '—',
      fileSize: type === 'VIDEO' ? '48.2 MB' : '6.8 MB',
      dateAdded: '2026-08-01',
      lastModified: '2026-08-05',
      rendererType: 'GPU',
      supportedMonitors: '2',
      favorite: true,
      rating: 4.5,
      filePath: `C:\\REX\\wallpapers\\${type.toLowerCase()}.rexwall`,
    },
    general: { ...DEFAULT_GENERAL },
    display: { ...DEFAULT_DISPLAY },
    playback: { ...DEFAULT_PLAYBACK },
    appearance: { ...DEFAULT_APPEARANCE },
    performance: { ...DEFAULT_PERFORMANCE },
    monitor: { ...DEFAULT_MONITOR },
    audio: { ...DEFAULT_AUDIO },
    interaction: { ...DEFAULT_INTERACTION },
    schedule: { ...DEFAULT_SCHEDULE },
    playlist: { ...DEFAULT_PLAYLIST },
    renderer: { ...RENDERER_DEFAULTS[type] },
    advanced: { ...DEFAULT_ADVANCED },
  }
}
