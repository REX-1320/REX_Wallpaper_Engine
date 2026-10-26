import { useEffect, useState } from 'react'
import { Monitor, Settings2, Info, FolderOpen, Rocket, Palette, Eraser } from 'lucide-react'
import { TooltipProvider } from '@/components/ui/tooltip'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyRow from '@/components/properties/PropertyRow'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { useAppState } from '@/hooks/useAppState'
import { formatStatus } from './statusFormat'
import { DEFAULT_DEFAULTS, FIT_MODES, sanitizeDefaults } from './defaults'
import './settings.css'

// The global Settings page. Every control here is backed by something the app
// actually reads:
//
//   startup.launchAtLogin    -> the OS login item (the tray reads the same)
//   startup.startMinimized    -> the login item's --background flag
//   startup.restoreWallpaper  -> the boot re-apply
//   tray.closeToTray          -> the window's close handler
//   ui.rememberWindowBounds   -> the window's bounds persistence
//   defaults.*                -> the baseline for newly imported wallpapers
//   engine.maxFps             -> the runtime's INIT (applies at engine start)
//   engine.hardwareAcceleration -> the runtime's INIT (applies at engine start)

const CLOSE_OPTIONS = [
  { value: 'tray', label: 'Keep in tray' },
  { value: 'quit', label: 'Quit Paiint' },
]

const FIT_OPTIONS = FIT_MODES.map((mode) => ({
  value: mode,
  label: { Cover: 'Fill', Contain: 'Fit', Stretch: 'Stretch', Center: 'Center', Tile: 'Tile' }[mode] ?? mode,
}))

const FPS_OPTIONS = [
  { value: '0', label: 'Off' },
  { value: '60', label: '60 fps' },
  { value: '30', label: '30 fps' },
  { value: '24', label: '24 fps' },
  { value: '15', label: '15 fps' },
]

function EngineStatus() {
  const [status, setStatus] = useState(null)
  const [stopped, setStopped] = useState(false)

  useEffect(() => {
    let alive = true
    const read = async () => {
      try {
        const res = await window.paiintDesktop?.getStatus?.()
        if (!alive) return
        if (!res?.available) {
          setStopped(true)
          return
        }
        setStopped(false)
        setStatus(res)
      } catch {
        if (alive) setStopped(true)
      }
    }
    read()
    const timer = setInterval(read, 2000)
    return () => {
      alive = false
      clearInterval(timer)
    }
  }, [])

  if (stopped) {
    return (
      <p className="insp-hint">
        The wallpaper engine is not running. Apply a video wallpaper to see it live.
      </p>
    )
  }

  const { rows, state } = formatStatus(status?.data)
  if (rows.length === 0) {
    return <p className="insp-hint">No scene is loaded.</p>
  }

  const capabilities = Array.isArray(status?.capabilities) ? status.capabilities : []

  return (
    <>
      <div className="settings-metrics">
        <div className="settings-metric">
          <span className="k">State</span>
          <span className="v">{state}</span>
        </div>
        {rows.map((row) => (
          <div className="settings-metric" key={row.label}>
            <span className="k">{row.label}</span>
            <span className="v">{row.value}</span>
          </div>
        ))}
      </div>
      {capabilities.length > 0 ? (
        <p className="insp-hint" title={capabilities.join(', ')}>
          The engine can apply {capabilities.length} live properties: {capabilities.join(', ')}
        </p>
      ) : null}
    </>
  )
}

function Files() {
  const [paths, setPaths] = useState(null)
  const [cleared, setCleared] = useState(null)

  useEffect(() => {
    window.paiintDesktop?.getPaths?.().then((p) => setPaths(p)).catch(() => {})
  }, [])

  const open = (target) => window.paiintDesktop?.openPath?.(target)
  const clear = async () => {
    const res = await window.paiintDesktop?.clearPosters?.()
    setCleared(res?.removed ?? 0)
  }

  if (!paths) return <p className="insp-hint">Loading…</p>

  return (
    <>
      <PropertyRow label="Library">
        <Button size="xs" variant="secondary" onClick={() => open('library')}>
          <FolderOpen size={13} /> Open
        </Button>
      </PropertyRow>
      <PropertyRow label="Poster cache">
        <Button size="xs" variant="secondary" onClick={() => open('posters')}>
          <FolderOpen size={13} /> Open
        </Button>
      </PropertyRow>
      <PropertyRow label="Logs">
        <Button size="xs" variant="secondary" onClick={() => open('logs')}>
          <FolderOpen size={13} /> Open
        </Button>
      </PropertyRow>
      <PropertyRow label="Crash reports">
        <Button size="xs" variant="secondary" onClick={() => open('crashes')}>
          <FolderOpen size={13} /> Open
        </Button>
      </PropertyRow>
      <PropertyRow label="Clear posters" description="Cached preview images are rebuilt on demand">
        <Button size="xs" variant="secondary" onClick={clear}>
          <Eraser size={13} /> {cleared == null ? 'Clear' : `${cleared} removed`}
        </Button>
      </PropertyRow>
    </>
  )
}

function About({ version }) {
  const [doc, setDoc] = useState(null)
  const [error, setError] = useState('')

  // The text is read from the files that shipped with this build (see
  // app:read-legal in main.cjs) — the page cannot drift from the package.
  const openDoc = async (target) => {
    setError('')
    try {
      const res = await window.paiintDesktop?.readLegal?.(target)
      if (res?.ok) setDoc({ title: res.title, text: res.text })
      else setError('That document could not be opened.')
    } catch {
      setError('That document could not be opened.')
    }
  }

  return (
    <div className="settings-about">
      <div className="settings-about-head">
        <div className="settings-about-mark" aria-hidden="true">
          P
        </div>
        <div className="settings-about-id">
          <span className="settings-about-name">Paiint</span>
          <span className="settings-about-tagline">Live wallpaper engine for Windows</span>
        </div>
        <span className="settings-about-version">{version ? `v${version}` : '—'}</span>
      </div>

      <p className="settings-about-blurb">
        Video, HDR and still wallpapers rendered on the desktop itself, behind your
        icons. When you quit, the last frame stays on the desktop as a still.
      </p>

      <PropertyGroup>
        <PropertyRow label="Publisher">
          <span>MoiNeX</span>
        </PropertyRow>
        <PropertyRow label="Engine">
          <span className="pl-mono">paiint-wallpaper-runtime</span>
        </PropertyRow>
        <PropertyRow label="Licence">
          <span>Proprietary</span>
        </PropertyRow>
        <PropertyRow label="Copyright">
          <span>© 2026 MoiNeX</span>
        </PropertyRow>
      </PropertyGroup>

      <p className="settings-about-note">
        <strong>Privacy:</strong> Paiint collects nothing and makes no network
        requests. Wallpapers, playlists and settings stay on this PC.
      </p>
      <p className="settings-about-note">
        Bundles FFmpeg (LGPL-2.1) and other open-source components. Every licence
        text — including all bundled JavaScript packages — ships with the app.
      </p>

      <div className="settings-about-legal">
        <Button variant="outline" size="sm" onClick={() => openDoc('eula')}>
          Licence agreement
        </Button>
        <Button variant="outline" size="sm" onClick={() => openDoc('privacy')}>
          Privacy policy
        </Button>
        <Button variant="outline" size="sm" onClick={() => openDoc('notices')}>
          Third-party notices
        </Button>
        <Button variant="outline" size="sm" onClick={() => openDoc('jsLicenses')}>
          JavaScript licences
        </Button>
      </div>
      {error ? <p className="settings-about-error">{error}</p> : null}

      <Dialog open={!!doc} onOpenChange={(open) => { if (!open) setDoc(null) }}>
        <DialogContent className="settings-legal-dialog">
          <DialogHeader>
            <DialogTitle>{doc?.title ?? ''}</DialogTitle>
            <DialogDescription>
              The exact text that shipped with this build.
            </DialogDescription>
          </DialogHeader>
          <pre className="settings-legal-text">{doc?.text ?? ''}</pre>
        </DialogContent>
      </Dialog>
    </div>
  )
}

export default function SettingsPage() {
  const [rememberBounds, setRememberBounds] = useAppState('ui.rememberWindowBounds', true)
  const [closeToTray, setCloseToTray] = useAppState('tray.closeToTray', true)
  const [startMinimized, setStartMinimized] = useAppState('startup.startMinimized', true)
  const [restoreWallpaper, setRestoreWallpaper] = useAppState('startup.restoreWallpaper', true)
  const [maxFps, setMaxFps] = useAppState('engine.maxFps', 0)
  const [hwDecode, setHwDecode] = useAppState('engine.hardwareAcceleration', true)
  const [defaults, setDefaults] = useState(() => sanitizeDefaults(DEFAULT_DEFAULTS))
  const [launchAtLogin, setLaunchAtLogin] = useState(false)
  const [version, setVersion] = useState('')

  useEffect(() => {
    window.paiintDesktop?.appState?.get?.('defaults').then((raw) => {
      if (raw) setDefaults(sanitizeDefaults(raw))
    }).catch(() => {})
    window.paiintDesktop?.appState?.get?.('version').then((v) => setVersion(v ?? '')).catch(() => {})
    window.paiintDesktop?.loginItem?.().then((s) => setLaunchAtLogin(!!s?.enabled)).catch(() => {})
  }, [])

  const saveDefaults = (next) => {
    setDefaults(next)
    window.paiintDesktop?.appState?.set?.('defaults', next)?.catch?.(() => {})
  }
  const setDefault = (group, key, value) => saveDefaults({ ...defaults, [group]: { ...defaults[group], [key]: value } })

  const toggleLogin = async (enabled) => {
    const res = await window.paiintDesktop?.loginItem?.(enabled)
    setLaunchAtLogin(!!res?.enabled)
  }

  return (
    <TooltipProvider delayDuration={200}>
      <div className="settings-page">
        <div className="settings-col">
          <PropertySection title="Startup" icon={<Rocket size={15} />}>
            <PropertyGroup>
              <PropertyToggle
                label="Launch at login"
                description="Start Paiint when you sign in to Windows"
                checked={launchAtLogin}
                onChange={toggleLogin}
              />
              <PropertyToggle
                label="Start minimised"
                description="Start in the tray without opening the window"
                checked={startMinimized !== false}
                onChange={(v) => setStartMinimized(v)}
              />
              <PropertyToggle
                label="Restore the last wallpaper at launch"
                description="Put the wallpaper back on the desktop when Paiint starts"
                checked={restoreWallpaper !== false}
                onChange={(v) => setRestoreWallpaper(v)}
              />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="Window" icon={<Monitor size={15} />}>
            <PropertyGroup>
              <PropertyToggle
                label="Remember window size and position"
                description="Restore where you left the window on the next launch"
                checked={rememberBounds !== false}
                onChange={(v) => setRememberBounds(v)}
              />
              <PropertyDropdown
                label="On close"
                description="What happens when you close the window"
                value={closeToTray === false ? 'quit' : 'tray'}
                options={CLOSE_OPTIONS}
                onChange={(v) => setCloseToTray(v === 'tray')}
              />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="Engine" icon={<Settings2 size={15} />}>
            <PropertyGroup>
              <PropertyDropdown
                label="Frame rate cap"
                description="Off follows the monitor. Applies when the wallpaper engine starts"
                value={String(Number(maxFps) || 0)}
                options={FPS_OPTIONS}
                onChange={(v) => setMaxFps(Number(v) || 0)}
              />
              <PropertyToggle
                label="Hardware video decoding"
                description="Off uses the CPU. Applies when the wallpaper engine starts"
                checked={hwDecode !== false}
                onChange={(v) => setHwDecode(v)}
              />
            </PropertyGroup>
            <PropertyGroup>
              <EngineStatus />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="New wallpapers" icon={<Palette size={15} />}>
            <PropertyGroup>
              <PropertyDropdown
                label="Default fit"
                description="How a newly imported wallpaper fills the screen"
                value={defaults.display.mode}
                options={FIT_OPTIONS}
                onChange={(v) => setDefault('display', 'mode', v)}
              />
              <PropertyToggle
                label="Start muted"
                checked={defaults.playback.mute}
                onChange={(v) => setDefault('playback', 'mute', v)}
              />
              <PropertySlider
                label="Default volume"
                value={defaults.playback.volume}
                onChange={(v) => setDefault('playback', 'volume', v)}
                min={0}
                max={100}
                suffix="%"
              />
              <PropertySlider
                label="Default brightness"
                value={defaults.appearance.brightness}
                onChange={(v) => setDefault('appearance', 'brightness', v)}
                min={0}
                max={200}
                step={5}
                suffix="%"
              />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="Files" icon={<FolderOpen size={15} />} defaultOpen={false}>
            <PropertyGroup>
              <Files />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="About" icon={<Info size={15} />} defaultOpen={false}>
            <About version={version} />
          </PropertySection>
        </div>
      </div>
    </TooltipProvider>
  )
}
