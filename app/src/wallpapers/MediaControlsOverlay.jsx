import { useEffect, useRef, useState } from 'react'
import {
  Camera,
  Check,
  Copy,
  Flag,
  Frame,
  Gauge,
  Heart,
  Info,
  ListMusic,
  Maximize,
  Minimize,
  MonitorCheck,
  Pause,
  PictureInPicture2,
  Play,
  Repeat,
  RotateCcw,
  Settings2,
  Shuffle,
  SkipBack,
  SkipForward,
  Square,
  StepBack,
  StepForward,
  Trash2,
  Volume2,
  VolumeX,
  Wand2,
  X,
} from 'lucide-react'
import { Slider } from '@/components/ui/slider'
import {
  AUDIO_SOURCES,
  CAMERAS,
  LIGHTINGS,
  OUTPUT_DEVICES,
  PLAYLIST_ICONS,
  QUALITIES,
  TYPE_META,
  WALLPAPER_MODES,
} from './registry'
import {
  deleteWallpaper,
  duplicateWallpaper,
  selectWallpaper,
  updateWallpaperProperty,
  useWallpaperStore,
} from './store'
import { getMedia } from './mediaBridge'
import { reapplyDesktopWallpaper } from './DesktopApply'
import './media-controls.css'

const HIDE_DELAY = 3000
const SPEED_PRESETS = [0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2]

function fmtTime(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return '0:00'
  const s = Math.floor(seconds)
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const sec = s % 60
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}` : `${m}:${String(sec).padStart(2, '0')}`
}

function fmtSpeed(v) {
  return `${Number(v).toFixed(v % 1 === 0 ? 0 : 2)}×`
}

const clamp = (v, min, max) => Math.min(max, Math.max(min, v))

function Btn({ onClick, label, children, className = '', active = false, disabled = false, onWheel }) {
  return (
    <button
      type="button"
      className={`mco-btn${active ? ' on' : ''}${className ? ` ${className}` : ''}`}
      aria-label={label}
      title={label}
      disabled={disabled}
      onWheel={onWheel}
      onClick={(e) => {
        e.stopPropagation()
        onClick?.(e)
      }}
    >
      {children}
    </button>
  )
}

function ToggleRow({ label, checked, onChange, disabled = false }) {
  return (
    <button
      type="button"
      className="mco-row"
      disabled={disabled}
      onClick={(e) => {
        e.stopPropagation()
        onChange(!checked)
      }}
    >
      <span className="mco-row-label">{label}</span>
      <span className={`mco-switch${checked ? ' on' : ''}`}>
        <span className="mco-switch-knob" />
      </span>
    </button>
  )
}

function SelectRow({ label, value, options, onChange }) {
  return (
    <label className="mco-row mco-row-select">
      <span className="mco-row-label">{label}</span>
      <select
        className="mco-select"
        value={value}
        onClick={(e) => e.stopPropagation()}
        onChange={(e) => {
          e.stopPropagation()
          onChange(e.target.value)
        }}
      >
        {options.map((opt) => (
          <option key={opt} value={opt}>
            {opt}
          </option>
        ))}
      </select>
    </label>
  )
}

function SliderRow({ label, value, min, max, step, suffix = '', onChange, disabled = false }) {
  return (
    <div className="mco-row mco-row-slider">
      <div className="mco-slider-head">
        <span className="mco-row-label">{label}</span>
        <span className="mco-slider-value">
          {value}
          {suffix}
        </span>
      </div>
      <Slider
        value={[value]}
        min={min}
        max={max}
        step={step}
        disabled={disabled}
        onValueChange={([v]) => onChange(v)}
      />
    </div>
  )
}

function Menu({ open, side = 'up', className = '', children }) {
  return (
    <div className={`mco-menu mco-menu-${side}${open ? ' open' : ''}${className ? ` ${className}` : ''}`}>
      {children}
    </div>
  )
}

function MenuTitle({ children }) {
  return <div className="mco-menu-title">{children}</div>
}

function MenuItem({ icon, label, onClick, active = false, danger = false }) {
  return (
    <button
      type="button"
      className={`mco-item${active ? ' on' : ''}${danger ? ' danger' : ''}`}
      onClick={(e) => {
        e.stopPropagation()
        onClick?.()
      }}
    >
      {icon ? <span className="mco-item-icon">{icon}</span> : null}
      <span className="mco-item-label">{label}</span>
      {active ? <Check size={12} className="mco-item-check" /> : null}
    </button>
  )
}

const KEY_HINTS = [
  ['Space', 'Play / Pause'],
  ['← / →', 'Seek ±5s'],
  ['Shift ← / →', 'Seek ±30s'],
  ['↑ / ↓', 'Volume'],
  ['M', 'Mute'],
  ['L', 'Loop'],
  ['F', 'Fullscreen'],
  ['Ctrl+R', 'Restart'],
]

export default function MediaControlsOverlay({ canvasRef }) {
  const { wallpapers, playlists, selectedId } = useWallpaperStore()
  const [visible, setVisible] = useState(false)
  const [time, setTime] = useState(0)
  const [duration, setDuration] = useState(0)
  const [buffered, setBuffered] = useState(0)
  const [scrub, setScrub] = useState(null)
  const [seekHover, setSeekHover] = useState(null)
  const [pop, setPop] = useState(null)
  const [fullscreen, setFullscreen] = useState(false)
  const [confirmDelete, setConfirmDelete] = useState(false)
  const [metaOpen, setMetaOpen] = useState(false)
  const [devStats, setDevStats] = useState({ dropped: 0, total: 0, fps: 0 })
  const [ab, setAb] = useState({ a: null, b: null })
  const [keyHintOpen, setKeyHintOpen] = useState(false)

  const rootRef = useRef(null)
  const seekBarRef = useRef(null)
  const previewRef = useRef(null)
  const hideTimer = useRef(null)
  const overRef = useRef(false)
  const popRef = useRef(null)
  const scrubRef = useRef(null)
  const keyHintRef = useRef(false)
  const selectedRef = useRef(null)
  const timeRef = useRef(0)
  const lastDrawRef = useRef(0)
  const abRef = useRef({ a: null, b: null })

  const selected = wallpapers.find((w) => w.id === selectedId) ?? null
  selectedRef.current = selected
  popRef.current = pop
  scrubRef.current = scrub
  keyHintRef.current = keyHintOpen
  abRef.current = ab
  const lastHoverFracRef = useRef(-1)
  const lastHoverSetAtRef = useRef(0)

  const meta = selected ? TYPE_META[selected.type] : null
  const TypeIcon = meta?.icon ?? Wand2
  const isVideo = selected?.type === 'VIDEO'
  const isImage = selected?.type === 'IMAGE'
  const isPlaying = selected ? selected.playback.playing && !selected.playback.paused : false
  const mediaEl = selected ? getMedia(selected.id) : null
  const showSeek = isVideo && mediaEl && duration > 0

  const set = (path, value) => {
    if (!selectedRef.current) return
    updateWallpaperProperty(selectedRef.current.id, path, value)
  }
  const play = () => {
    set(['playback', 'playing'], true)
    set(['playback', 'paused'], false)
  }
  const pause = () => set(['playback', 'paused'], true)
  const togglePlay = () => {
    const w = selectedRef.current
    if (!w) return
    const playing = w.playback.playing && !w.playback.paused
    if (playing) {
      set(['playback', 'paused'], true)
    } else {
      set(['playback', 'playing'], true)
      set(['playback', 'paused'], false)
    }
  }
  const seekTo = (t) => {
    const w = selectedRef.current
    if (!w || !w.playback.seekTimeline) return
    const el = getMedia(w.id)
    if (!el) return
    el.currentTime = clamp(t, 0, el.duration || 0)
  }
  const step = (delta) => {
    const w = selectedRef.current
    if (!w || !w.playback.seekTimeline) return
    const el = getMedia(w.id)
    if (!el) return
    el.currentTime = clamp(el.currentTime + delta, 0, el.duration || 0)
  }
  const restart = () => {
    seekTo(0)
    set(['playback', 'startPosition'], 0)
    play()
  }
  const stop = () => {
    seekTo(0)
    set(['playback', 'playing'], false)
    set(['playback', 'paused'], true)
  }
  const cycle = (dir) => {
    if (!wallpapers.length) return
    const idx = wallpapers.findIndex((w) => w.id === selectedId)
    selectWallpaper(wallpapers[(idx + dir + wallpapers.length) % wallpapers.length].id)
  }
  const randomPick = () => {
    if (wallpapers.length < 2) return
    let next = wallpapers[Math.floor(Math.random() * wallpapers.length)]
    if (next.id === selectedId) next = wallpapers[(wallpapers.findIndex((w) => w.id === selectedId) + 1) % wallpapers.length]
    selectWallpaper(next.id)
  }
  const setVolume = (v) => {
    const vol = Math.round(clamp(v, 0, 100))
    set(['playback', 'volume'], vol)
    if (vol > 0 && selectedRef.current?.playback.mute) set(['playback', 'mute'], false)
  }
  const toggleMute = () => set(['playback', 'mute'], !selectedRef.current?.playback.mute)

  const show = () => {
    setVisible(true)
    clearTimeout(hideTimer.current)
    const hideCheck = () => {
      if (popRef.current || scrubRef.current || keyHintRef.current) {
        hideTimer.current = setTimeout(hideCheck, HIDE_DELAY)
      } else {
        setVisible(false)
      }
    }
    hideTimer.current = setTimeout(hideCheck, HIDE_DELAY)
  }

  useEffect(() => {
    if (!visible) return
    return () => clearTimeout(hideTimer.current)
  }, [visible])

  useEffect(() => {
    const bar = seekBarRef.current
    if (!bar) return
    const onWheel = (e) => {
      if (!overRef.current) return
      e.preventDefault()
      step(e.deltaY > 0 ? 5 : -5)
      show()
    }
    bar.addEventListener('wheel', onWheel, { passive: false })
    return () => bar.removeEventListener('wheel', onWheel)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedId])

  useEffect(() => {
    const onFullscreenChange = () => setFullscreen(!!document.fullscreenElement)
    document.addEventListener('fullscreenchange', onFullscreenChange)
    return () => document.removeEventListener('fullscreenchange', onFullscreenChange)
  }, [])

  useEffect(() => {
    let raf
    let frameCount = 0
    let lastFpsAt = performance.now()
    let lastTimeSent = -1
    const tick = () => {
      const w = selectedRef.current
      const el = w ? getMedia(w.id) : null
      if (el && el.tagName === 'VIDEO') {
        if (Math.abs(el.currentTime - lastTimeSent) > 0.05 || scrubRef.current) {
          timeRef.current = el.currentTime
          lastTimeSent = el.currentTime
          if (!scrubRef.current) setTime(el.currentTime)
        }
        const dur = el.duration
        if (dur !== duration) setDuration(dur)
        if (el.buffered.length > 0) {
          const buf = el.buffered.end(el.buffered.length - 1)
          setBuffered((prev) => (Math.abs(prev - buf) > 0.1 ? buf : prev))
        }
        if (abRef.current.b != null && el.currentTime >= abRef.current.b) {
          el.currentTime = abRef.current.a ?? 0
        }
        frameCount += 1
        const now = performance.now()
        if (now - lastFpsAt > 1000) {
          const fps = Math.round((frameCount * 1000) / (now - lastFpsAt))
          frameCount = 0
          lastFpsAt = now
          const q = el.getVideoPlaybackQuality ? el.getVideoPlaybackQuality() : null
          const dropped = q?.droppedVideoFrames ?? 0
          const total = q?.totalVideoFrames ?? 0
          setDevStats((prev) =>
            prev.dropped !== dropped || prev.total !== total || prev.fps !== fps
              ? { dropped, total, fps }
              : prev
          )
        }
        if (seekHover && el.readyState >= 2) {
          const now2 = performance.now()
          if (now2 - lastDrawRef.current > 50) {
            lastDrawRef.current = now2
            const c = previewRef.current
            if (c) c.getContext('2d')?.drawImage(el, 0, 0, c.width, c.height)
          }
        }
      }
      raf = requestAnimationFrame(tick)
    }
    raf = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(raf)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedId, duration, seekHover])

  useEffect(() => {
    const onKey = (e) => {
      if (!overRef.current) return
      const t = e.target
      if (t instanceof HTMLElement && t.closest('input, textarea, select, [contenteditable]')) return
      if (e.ctrlKey || e.metaKey) {
        if ((e.key === 'r' || e.key === 'R') && !e.altKey) {
          e.preventDefault()
          restart()
        }
        return
      }
      if (e.altKey) return
      const w = selectedRef.current
      if (!w) return
      const el = getMedia(w.id)
      switch (e.key) {
        case ' ':
          e.preventDefault()
          if (t instanceof HTMLElement && t.closest('button')) return
          togglePlay()
          break
        case 'ArrowLeft':
          e.preventDefault()
          step(e.shiftKey ? -30 : -5)
          break
        case 'ArrowRight':
          e.preventDefault()
          step(e.shiftKey ? 30 : 5)
          break
        case 'ArrowUp':
          e.preventDefault()
          setVolume(w.playback.volume + 10)
          break
        case 'ArrowDown':
          e.preventDefault()
          setVolume(w.playback.volume - 10)
          break
        case 'm':
        case 'M':
          toggleMute()
          break
        case 'l':
        case 'L':
          set(['playback', 'loop'], !w.playback.loop)
          break
        case 'f':
        case 'F':
          toggleFullscreen()
          break
        case 'Escape':
          if (popRef.current) setPop(null)
          else if (keyHintOpen) setKeyHintOpen(false)
          break
        default:
          break
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedId])

  const toggleFullscreen = () => {
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {})
    else canvasRef.current?.requestFullscreen?.().catch(() => {})
  }

  const togglePiP = async () => {
    const el = getMedia(selectedRef.current?.id)
    if (!el) return
    if (document.pictureInPictureElement) await document.exitPictureInPicture().catch(() => {})
    else el.requestPictureInPicture?.().catch(() => {})
  }

  const screenshot = () => {
    const el = getMedia(selectedRef.current?.id)
    if (!el) return
    const c = document.createElement('canvas')
    c.width = el.videoWidth || 1280
    c.height = el.videoHeight || 720
    c.getContext('2d').drawImage(el, 0, 0)
    const a = document.createElement('a')
    a.href = c.toDataURL('image/png')
    a.download = `${selectedRef.current?.info.name ?? 'wallpaper'}-frame.png`
    a.click()
  }

  const applyToDesktop = () => {
    reapplyDesktopWallpaper()
  }

  const seekFromClientX = (clientX) => {
    const bar = seekBarRef.current
    if (!bar) return 0
    const rect = bar.getBoundingClientRect()
    return clamp((clientX - rect.left) / rect.width, 0, 1)
  }

  const pct = (frac) => `${(frac * 100).toFixed(2)}%`
  const showPct = scrub ? pct(scrub.frac) : duration > 0 ? pct(time / duration) : '0%'
  const bufferedPct = duration > 0 ? pct(Math.min(1, buffered / duration)) : '0%'
  const seekTipTime = duration * (seekHover?.frac ?? 0)
  const handleOpen = (key) => setPop((prev) => (prev === key ? null : key))

  const closePop = () => setPop(null)

  if (!selected) return null

  const p = selected.playback
  const d = selected.display

  const rendererMenu = () => {
    if (selected.type === 'WEB') {
      return (
        <Menu open={pop === 'renderer'} side="down">
          <MenuTitle>Web Wallpaper</MenuTitle>
          <ToggleRow label="JavaScript" checked={selected.renderer.javascript} onChange={(v) => set(['renderer', 'javascript'], v)} />
          <ToggleRow label="DevTools" checked={selected.renderer.devTools} onChange={(v) => set(['renderer', 'devTools'], v)} />
          <ToggleRow label="Cache" checked={selected.renderer.cache} onChange={(v) => set(['renderer', 'cache'], v)} />
          <SliderRow label="Zoom" value={selected.renderer.zoom} min={25} max={300} step={5} suffix="%" onChange={(v) => set(['renderer', 'zoom'], v)} />
        </Menu>
      )
    }
    if (selected.type === 'SCENE') {
      return (
        <Menu open={pop === 'renderer'} side="down">
          <MenuTitle>Scene</MenuTitle>
          <SelectRow label="Camera" value={selected.renderer.camera} options={CAMERAS} onChange={(v) => set(['renderer', 'camera'], v)} />
          <SelectRow label="Lighting" value={selected.renderer.lighting} options={LIGHTINGS} onChange={(v) => set(['renderer', 'lighting'], v)} />
          <SelectRow label="Quality" value={selected.renderer.quality} options={QUALITIES} onChange={(v) => set(['renderer', 'quality'], v)} />
          <ToggleRow label="Particles" checked={selected.renderer.particles} onChange={(v) => set(['renderer', 'particles'], v)} />
          <ToggleRow label="Physics" checked={selected.renderer.physics} onChange={(v) => set(['renderer', 'physics'], v)} />
          <ToggleRow label="Post Processing" checked={selected.renderer.postProcessing} onChange={(v) => set(['renderer', 'postProcessing'], v)} />
        </Menu>
      )
    }
    if (selected.type === 'SHADER') {
      return (
        <Menu open={pop === 'renderer'} side="down">
          <MenuTitle>Shader</MenuTitle>
          <div className="mco-row">
            <span className="mco-row-label">Shader File</span>
            <span className="mco-chip">{selected.renderer.shaderFile}</span>
          </div>
          <div className="mco-row">
            <span className="mco-row-label">FPS</span>
            <span className="mco-chip">{selected.renderer.fps} FPS</span>
          </div>
          <ToggleRow label="Pause Animation" checked={p.paused} onChange={(v) => set(['playback', 'paused'], v)} />
          <ToggleRow label="Play (Resume)" checked={isPlaying} onChange={(v) => (v ? play() : pause())} />
        </Menu>
      )
    }
    if (selected.type === 'AUDIO_VISUALIZER') {
      const r = selected.renderer
      return (
        <Menu open={pop === 'renderer'} side="down">
          <MenuTitle>Audio Visualizer</MenuTitle>
          <SelectRow label="Audio Source" value={r.audioSource} options={AUDIO_SOURCES} onChange={(v) => set(['renderer', 'audioSource'], v)} />
          <SelectRow label="Output Device" value={selected.audio.outputDevice} options={OUTPUT_DEVICES} onChange={(v) => set(['audio', 'outputDevice'], v)} />
          <SliderRow label="Sensitivity" value={r.sensitivity} min={0} max={100} suffix="%" onChange={(v) => set(['renderer', 'sensitivity'], v)} />
          <ToggleRow label="Bars" checked={r.bars} onChange={(v) => set(['renderer', 'bars'], v)} />
          <ToggleRow label="Wave" checked={r.wave} onChange={(v) => set(['renderer', 'wave'], v)} />
          <ToggleRow label="Spectrum" checked={r.spectrum} onChange={(v) => set(['renderer', 'spectrum'], v)} />
          <ToggleRow label="Peak Hold" checked={r.peakHold} onChange={(v) => set(['renderer', 'peakHold'], v)} />
        </Menu>
      )
    }
    return null
  }

  const displayMenu = () => {
    if (isImage || isVideo) {
      return (
        <Menu open={pop === 'fit'}>
          <MenuTitle>Display</MenuTitle>
          <div className="mco-modes">
            {WALLPAPER_MODES.map((mode) => (
              <button
                type="button"
                key={mode}
                className={`mco-mode${d.mode === mode ? ' on' : ''}`}
                onClick={(e) => {
                  e.stopPropagation()
                  set(['display', 'mode'], mode)
                }}
              >
                {mode}
              </button>
            ))}
          </div>
          {isVideo ? (
            <button type="button" className="mco-item" onClick={(e) => { e.stopPropagation(); togglePiP() }}>
              <span className="mco-item-icon"><PictureInPicture2 size={13} /></span>
              <span className="mco-item-label">Picture-in-Picture</span>
            </button>
          ) : null}
          {isImage ? (
            <>
              <SliderRow label="Zoom" value={d.scale} min={25} max={300} step={5} suffix="%" onChange={(v) => set(['display', 'scale'], v)} />
              <SliderRow label="Pan X" value={d.offsetX} min={-200} max={200} step={5} suffix="px" onChange={(v) => set(['display', 'offsetX'], v)} />
              <SliderRow label="Pan Y" value={d.offsetY} min={-200} max={200} step={5} suffix="px" onChange={(v) => set(['display', 'offsetY'], v)} />
              <SliderRow label="Rotate" value={d.rotation} min={-180} max={180} step={1} suffix="°" onChange={(v) => set(['display', 'rotation'], v)} />
              <ToggleRow label="Flip Horizontal" checked={d.flipH} onChange={(v) => set(['display', 'flipH'], v)} />
              <ToggleRow label="Flip Vertical" checked={d.flipV} onChange={(v) => set(['display', 'flipV'], v)} />
              <MenuItem
                icon={<RotateCcw size={13} />}
                label="Reset View"
                onClick={() => {
                  set(['display', 'scale'], 100)
                  set(['display', 'offsetX'], 0)
                  set(['display', 'offsetY'], 0)
                  set(['display', 'rotation'], 0)
                  set(['display', 'flipH'], false)
                  set(['display', 'flipV'], false)
                }}
              />
            </>
          ) : null}
        </Menu>
      )
    }
    return null
  }

  const settingsMenu = () => (
    <Menu open={pop === 'settings'}>
      <MenuTitle>Playback Settings</MenuTitle>
      <ToggleRow label="Loop" checked={p.loop} onChange={(v) => set(['playback', 'loop'], v)} />
      <ToggleRow label="Autoplay" checked={p.autoplay} onChange={(v) => set(['playback', 'autoplay'], v)} />
      <ToggleRow label="Random Start" checked={p.randomStart} onChange={(v) => set(['playback', 'randomStart'], v)} />
      <ToggleRow label="Resume Previous" checked={p.resumePrevious} onChange={(v) => set(['playback', 'resumePrevious'], v)} />
      {isVideo ? <ToggleRow label="Seek Timeline" checked={p.seekTimeline} onChange={(v) => set(['playback', 'seekTimeline'], v)} /> : null}
      <ToggleRow label="Normalize Volume" checked={p.normalizeVolume} onChange={(v) => set(['playback', 'normalizeVolume'], v)} />
    </Menu>
  )

  const volumeMenu = () => (
    <Menu open={pop === 'volume'}>
      <MenuTitle>Audio</MenuTitle>
      <div className="mco-volume-row">
        <Btn label={p.mute ? 'Unmute' : 'Mute'} onClick={toggleMute} active={p.mute}>
          {p.mute || p.volume === 0 ? <VolumeX size={14} /> : <Volume2 size={14} />}
        </Btn>
        <Slider
          value={[p.volume]}
          min={0}
          max={100}
          step={1}
          onValueChange={([v]) => setVolume(v)}
        />
        <span className="mco-volume-pct">{p.mute ? 0 : p.volume}%</span>
      </div>
      <ToggleRow label="Remember Volume" checked={p.resumePrevious} onChange={(v) => set(['playback', 'resumePrevious'], v)} />
      <ToggleRow label="Normalize Volume" checked={p.normalizeVolume} onChange={(v) => set(['playback', 'normalizeVolume'], v)} />
      <SelectRow label="Output Device" value={selected.audio.outputDevice} options={OUTPUT_DEVICES} onChange={(v) => set(['audio', 'outputDevice'], v)} />
      <div className="mco-hint">Output device applies to the desktop runtime only — browsers cannot select audio output.</div>
    </Menu>
  )

  const speedMenu = () => (
    <Menu open={pop === 'speed'}>
      <MenuTitle>Playback Speed</MenuTitle>
      <div className="mco-speeds">
        {SPEED_PRESETS.map((s) => (
          <button
            type="button"
            key={s}
            className={`mco-speed${Math.abs(p.speed - s) < 0.01 ? ' on' : ''}`}
            onClick={(e) => {
              e.stopPropagation()
              set(['playback', 'speed'], s)
            }}
          >
            {fmtSpeed(s)}
          </button>
        ))}
      </div>
      <SliderRow label="Custom" value={p.speed} min={0.25} max={2} step={0.05} suffix="×" onChange={(v) => set(['playback', 'speed'], v)} />
    </Menu>
  )

  const infoMenu = () => (
    <Menu open={pop === 'info'} side="down">
      <MenuTitle>Information</MenuTitle>
      <div className="mco-info-grid">
        <span className="mco-info-key">Current Time</span>
        <span className="mco-info-val">{fmtTime(isVideo && mediaEl ? mediaEl.currentTime : time)}</span>
        <span className="mco-info-key">Duration</span>
        <span className="mco-info-val">{selected.info.duration === '—' ? fmtTime(duration) : selected.info.duration}</span>
        <span className="mco-info-key">Resolution</span>
        <span className="mco-info-val">
          {isVideo && mediaEl?.videoWidth ? `${mediaEl.videoWidth} × ${mediaEl.videoHeight}` : selected.info.resolution}
        </span>
        <span className="mco-info-key">FPS</span>
        <span className="mco-info-val">{isVideo && devStats.fps ? `${devStats.fps} (measured)` : selected.info.fps}</span>
        <span className="mco-info-key">Codec</span>
        <span className="mco-info-val">{selected.info.codec}</span>
        <span className="mco-info-key">Bitrate</span>
        <span className="mco-info-val">{isVideo ? 'Variable' : '—'}</span>
        <span className="mco-info-key">Renderer</span>
        <span className="mco-info-val">{selected.performance.renderer} · {selected.info.rendererType}</span>
        <span className="mco-info-key">GPU Decode</span>
        <span className="mco-info-val">{isVideo ? (selected.renderer.hardwareDecode ? 'Yes' : 'No') : '—'}</span>
        <span className="mco-info-key">HW Accel</span>
        <span className="mco-info-val">{selected.renderer.hardwareDecode ? 'Enabled' : 'Disabled'}</span>
        <span className="mco-info-key">Dropped Frames</span>
        <span className="mco-info-val">{isVideo ? devStats.dropped : '—'}</span>
      </div>
    </Menu>
  )

  const actionsMenu = () => (
    <Menu open={pop === 'actions'} side="down">
      <MenuTitle>Quick Actions</MenuTitle>
      <MenuItem icon={<MonitorCheck size={13} />} label="Apply Wallpaper" onClick={applyToDesktop} />
      <MenuItem
        icon={<Heart size={13} />}
        label="Favorite"
        active={selected.general.favorite}
        onClick={() => set(['general', 'favorite'], !selected.general.favorite)}
      />
      <MenuTitle>Add to playlist</MenuTitle>
      {playlists.map((pl) => {
        const Icon = PLAYLIST_ICONS[pl.icon] ?? ListMusic
        return (
          <MenuItem
            key={pl.id}
            icon={<Icon size={13} />}
            label={pl.name}
            active={selected.playlist.currentPlaylist === pl.name}
            onClick={() => set(['playlist', 'currentPlaylist'], pl.name)}
          />
        )
      })}
      <MenuTitle>Manage</MenuTitle>
      <MenuItem icon={<Copy size={13} />} label="Duplicate" onClick={() => duplicateWallpaper(selected.id)} />
      <MenuItem
        icon={<Trash2 size={13} />}
        label={confirmDelete ? 'Confirm delete?' : 'Delete'}
        danger={confirmDelete}
        onClick={() => {
          if (confirmDelete) {
            deleteWallpaper(selected.id)
            setConfirmDelete(false)
            closePop()
          } else {
            setConfirmDelete(true)
            setTimeout(() => setConfirmDelete(false), 2500)
          }
        }}
      />
    </Menu>
  )

  const devMenu = () => (
    <Menu open={pop === 'dev'} side="down">
      <MenuTitle>Advanced</MenuTitle>
      {isVideo ? (
        <>
          <MenuItem icon={<Camera size={13} />} label="Screenshot Frame" onClick={screenshot} />
          <MenuItem icon={<Info size={13} />} label="Open Metadata" onClick={() => setMetaOpen(true)} />
          <MenuTitle>Frame Step</MenuTitle>
          <div className="mco-step-row">
            <Btn label="Step back 1 frame" onClick={() => step(-1 / 30)}>
              <StepBack size={14} />
            </Btn>
            <Btn label="Step forward 1 frame" onClick={() => step(1 / 30)}>
              <StepForward size={14} />
            </Btn>
          </div>
          <MenuTitle>A-B Loop</MenuTitle>
          <div className="mco-step-row">
            <Btn label="Set point A" active={ab.a != null} onClick={() => {
              const el = getMedia(selected.id)
              setAb((prev) => ({ ...prev, a: el?.currentTime ?? prev.a }))
            }}>
              <Flag size={14} /> A
            </Btn>
            <Btn label="Set point B" active={ab.b != null} onClick={() => {
              const el = getMedia(selected.id)
              setAb((prev) => ({ ...prev, b: el?.currentTime ?? prev.b }))
            }}>
              <Flag size={14} /> B
            </Btn>
            <Btn label="Clear A-B loop" onClick={() => setAb({ a: null, b: null })}>
              <X size={14} />
            </Btn>
          </div>
          <MenuTitle>Developer Statistics</MenuTitle>
          <div className="mco-info-grid">
            <span className="mco-info-key">FPS</span>
            <span className="mco-info-val">{devStats.fps || '—'}</span>
            <span className="mco-info-key">Dropped Frames</span>
            <span className="mco-info-val">{devStats.dropped}</span>
            <span className="mco-info-key">Total Frames</span>
            <span className="mco-info-val">{devStats.total}</span>
          </div>
        </>
      ) : (
        <div className="mco-hint">Advanced tools are available for video wallpapers.</div>
      )}
    </Menu>
  )

  return (
    <>
      <div
        ref={rootRef}
        className={`media-controls-overlay${visible ? ' visible' : ''}`}
        onMouseMove={show}
        onMouseEnter={() => {
          overRef.current = true
          show()
        }}
        onMouseLeave={() => {
          overRef.current = false
          if (popRef.current || scrubRef.current || keyHintRef.current) return
          setScrub(null)
          setSeekHover(null)
          clearTimeout(hideTimer.current)
          hideTimer.current = setTimeout(() => setVisible(false), 350)
        }}
        onClick={() => {
          if (popRef.current) setPop(null)
        }}
      >
        {pop || keyHintOpen ? (
          <div
            className="mco-pop-catcher"
            onClick={(e) => {
              e.stopPropagation()
              setPop(null)
              setKeyHintOpen(false)
            }}
          />
        ) : null}
        <div className="mco-scrim" />

        <div className="mco-topbar" onClick={(e) => e.stopPropagation()}>
          <div className="mco-title">
            <span className="mco-title-name">{selected.info.name}</span>
            <span className="mco-title-badge">{meta.label}</span>
            {isVideo && mediaEl?.videoWidth ? (
              <span className="mco-title-chip">{mediaEl.videoWidth} × {mediaEl.videoHeight}</span>
            ) : null}
          </div>
          <div className="mco-topbar-actions">
            <div className="mco-btnwrap">
              <Btn label={`${meta.label} controls`} active={pop === 'renderer'} onClick={() => handleOpen('renderer')}>
                <TypeIcon size={15} />
              </Btn>
              {rendererMenu()}
            </div>
            <div className="mco-btnwrap">
              <Btn label="Shortcuts" active={keyHintOpen} onClick={() => setKeyHintOpen((v) => !v)}>
                <Wand2 size={14} />
              </Btn>
              <div className={`mco-menu mco-menu-down${keyHintOpen ? ' open' : ''} mco-menu-wide`}>
                <MenuTitle>Keyboard Shortcuts</MenuTitle>
                {KEY_HINTS.map(([key, label]) => (
                  <div className="mco-shortcut-row" key={key}>
                    <span className="mco-kbd">{key}</span>
                    <span>{label}</span>
                  </div>
                ))}
              </div>
            </div>
            <div className="mco-btnwrap">
              <Btn label="Information" active={pop === 'info'} onClick={() => handleOpen('info')}>
                <Info size={15} />
              </Btn>
              {infoMenu()}
            </div>
            <div className="mco-btnwrap">
              <Btn label="Quick actions" active={pop === 'actions'} onClick={() => handleOpen('actions')}>
                <Heart size={15} />
              </Btn>
              {actionsMenu()}
            </div>
            {isVideo ? (
              <div className="mco-btnwrap">
                <Btn label="Advanced" active={pop === 'dev'} onClick={() => handleOpen('dev')}>
                  <Gauge size={15} />
                </Btn>
                {devMenu()}
              </div>
            ) : null}
            <div className="mco-btnwrap">
              <Btn label={fullscreen ? 'Exit fullscreen' : 'Fullscreen'} active={fullscreen} onClick={toggleFullscreen}>
                {fullscreen ? <Minimize size={15} /> : <Maximize size={15} />}
              </Btn>
            </div>
          </div>
        </div>

        <div className="mco-bottombar" onClick={(e) => e.stopPropagation()}>
          <div className="mco-cluster">
            <Btn label="Replay" onClick={restart}>
              <RotateCcw size={15} />
            </Btn>
            <Btn label="Previous wallpaper" onClick={() => cycle(-1)}>
              <SkipBack size={16} />
            </Btn>
            <Btn label={isPlaying ? 'Pause' : 'Play'} className="mco-btn-primary" onClick={togglePlay}>
              {isPlaying ? <Pause size={17} /> : <Play size={17} />}
            </Btn>
            <Btn label="Next wallpaper" onClick={() => cycle(1)}>
              <SkipForward size={16} />
            </Btn>
            <Btn label="Random wallpaper" onClick={randomPick}>
              <Shuffle size={15} />
            </Btn>
            <Btn label="Stop" onClick={stop}>
              <Square size={14} />
            </Btn>
          </div>

          {showSeek ? (
            <div
              ref={seekBarRef}
              className={`mco-seek${scrub ? ' dragging' : ''}`}
              onPointerDown={(e) => {
                e.preventDefault()
                const frac = seekFromClientX(e.clientX)
                seekBarRef.current?.setPointerCapture(e.pointerId)
                setScrub({ active: true, frac })
                show()
              }}
              onPointerMove={(e) => {
                const frac = seekFromClientX(e.clientX)
                if (scrub) {
                  setScrub({ active: true, frac })
                } else if (e.buttons === 0) {
                  const now = performance.now()
                  if (Math.abs(frac - lastHoverFracRef.current) > 0.001 || now - lastHoverSetAtRef.current > 150) {
                    lastHoverFracRef.current = frac
                    lastHoverSetAtRef.current = now
                    setSeekHover({ frac })
                  }
                }
              }}
              onPointerUp={(e) => {
                if (!scrub) return
                seekBarRef.current?.releasePointerCapture(e.pointerId)
                seekTo(scrub.frac * duration)
                setScrub(null)
                setSeekHover(null)
              }}
              onPointerLeave={() => setSeekHover(null)}
            >
              <span className="mco-seek-time">{fmtTime(scrub ? scrub.frac * duration : time)}</span>
              <div className="mco-seek-track">
                <div className="mco-seek-buffered" style={{ width: bufferedPct }} />
                <div className="mco-seek-fill" style={{ width: showPct }} />
                <div className="mco-seek-thumb" style={{ left: showPct }} />
                {seekHover ? (
                  <div
                    className="mco-seek-tip"
                    style={{ left: pct(seekHover.frac) }}
                  >
                    <canvas ref={previewRef} width={160} height={90} />
                    <span>{fmtTime(seekTipTime)}</span>
                  </div>
                ) : null}
              </div>
              <span className="mco-seek-time">{fmtTime(duration)}</span>
            </div>
          ) : (
            <div className="mco-seek-spacer" />
          )}

          <div className="mco-cluster">
            <div className="mco-btnwrap">
              <Btn label="Loop" active={p.loop} onClick={() => set(['playback', 'loop'], !p.loop)}>
                <Repeat size={15} />
              </Btn>
            </div>
            <div className="mco-btnwrap">
              <Btn label="Playback speed" active={pop === 'speed'} onClick={() => handleOpen('speed')}>
                <Gauge size={15} />
                <span className="mco-speed-label">{fmtSpeed(p.speed)}</span>
              </Btn>
              {speedMenu()}
            </div>
            <div className="mco-btnwrap">
              <Btn
                label={p.mute || p.volume === 0 ? 'Unmute' : 'Mute'}
                active={p.mute || p.volume === 0}
                onClick={toggleMute}
                onWheel={(e) => {
                  e.stopPropagation()
                  setVolume(p.volume + (e.deltaY > 0 ? -5 : 5))
                }}
              >
                {p.mute || p.volume === 0 ? <VolumeX size={15} /> : <Volume2 size={15} />}
              </Btn>
              {volumeMenu()}
            </div>
            {(isImage || isVideo) ? (
              <div className="mco-btnwrap">
                <Btn label="Display" active={pop === 'fit'} onClick={() => handleOpen('fit')}>
                  <Frame size={15} />
                </Btn>
                {displayMenu()}
              </div>
            ) : null}
            <div className="mco-btnwrap">
              <Btn label="Settings" active={pop === 'settings'} onClick={() => handleOpen('settings')}>
                <Settings2 size={15} />
              </Btn>
              {settingsMenu()}
            </div>
          </div>
        </div>
      </div>

      {!isPlaying ? (
        <button
          type="button"
          className="mco-bigplay"
          aria-label="Play"
          onClick={(e) => {
            e.stopPropagation()
            play()
          }}
        >
          <Play size={26} />
        </button>
      ) : null}

      {metaOpen ? (
        <div className="mco-modal-backdrop" onClick={() => setMetaOpen(false)}>
          <div className="mco-modal" onClick={(e) => e.stopPropagation()}>
            <div className="mco-modal-head">
              <span className="mco-modal-title">Metadata</span>
              <Btn label="Close" onClick={() => setMetaOpen(false)}>
                <X size={14} />
              </Btn>
            </div>
            <div className="mco-info-grid">
              {Object.entries(selected.info).map(([key, value]) => (
                <div key={key} className="mco-meta-row">
                  <span className="mco-info-key">{key}</span>
                  <span className="mco-info-val">{String(value)}</span>
                </div>
              ))}
            </div>
          </div>
        </div>
      ) : null}
    </>
  )
}
