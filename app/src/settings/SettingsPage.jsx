import { useEffect, useState } from 'react'
import { Monitor, Settings2, FolderOpen, Rocket, Palette, Eraser, Bug } from 'lucide-react'
import { TooltipProvider } from '@/components/ui/tooltip'
import { Button } from '@/components/ui/button'
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
        Engine offline. Apply a video wallpaper to start it.
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
      <PropertyRow label="Clear posters" description="Preview cache. Rebuilt on demand">
        <Button size="xs" variant="secondary" onClick={clear}>
          <Eraser size={13} /> {cleared == null ? 'Clear' : `${cleared} removed`}
        </Button>
      </PropertyRow>
    </>
  )
}

export default function SettingsPage() {
  const [rememberBounds, setRememberBounds] = useAppState('ui.rememberWindowBounds', true)
  const [closeToTray, setCloseToTray] = useAppState('tray.closeToTray', false)
  const [startMinimized, setStartMinimized] = useAppState('startup.startMinimized', true)
  const [restoreWallpaper, setRestoreWallpaper] = useAppState('startup.restoreWallpaper', true)
  const [maxFps, setMaxFps] = useAppState('engine.maxFps', 0)
  const [hwDecode, setHwDecode] = useAppState('engine.hardwareAcceleration', true)
  const [snapshotOnQuit, setSnapshotOnQuit] = useAppState('engine.snapshotOnQuit', true)
  const [audioOnlyWhenClosed, setAudioOnlyWhenClosed] = useAppState('engine.audioOnlyWhenWindowClosed', false)
  const [diagnosticsEnabled, setDiagnosticsEnabled] = useAppState('diagnostics.enabled', false)
  const [diagStatus, setDiagStatus] = useState(null)
  const [defaults, setDefaults] = useState(() => sanitizeDefaults(DEFAULT_DEFAULTS))
  const [launchAtLogin, setLaunchAtLogin] = useState(false)

  useEffect(() => {
    window.paiintDesktop?.appState?.get?.('defaults').then((raw) => {
      if (raw) setDefaults(sanitizeDefaults(raw))
    }).catch(() => {})
    window.paiintDesktop?.loginItem?.().then((s) => setLaunchAtLogin(!!s?.enabled)).catch(() => {})
  }, [])

  useEffect(() => {
    let alive = true
    const read = () => {
      window.paiintDesktop?.diagnostics?.status?.()
        .then((s) => { if (alive) setDiagStatus(s) })
        .catch(() => {})
    }
    read()
    const timer = setInterval(read, 5000)
    return () => {
      alive = false
      clearInterval(timer)
    }
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
                description="Start with your Windows session."
                checked={launchAtLogin}
                onChange={toggleLogin}
              />
              <PropertyToggle
                label="Start minimised"
                description="Boot to tray. No window."
                checked={startMinimized !== false}
                onChange={(v) => setStartMinimized(v)}
              />
              <PropertyToggle
                label="Restore the last wallpaper at launch"
                description="Re-apply the last scene."
                checked={restoreWallpaper !== false}
                onChange={(v) => setRestoreWallpaper(v)}
              />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="Window" icon={<Monitor size={15} />}>
            <PropertyGroup>
              <PropertyToggle
                label="Remember window size and position"
                description="Persist size and position."
                checked={rememberBounds !== false}
                onChange={(v) => setRememberBounds(v)}
              />
              <PropertyToggle
                label="Quit Paiint when the window closes"
                description="Stop engine. Keep last frame. Off = stay in tray."
                checked={closeToTray === false}
                onChange={(v) => setCloseToTray(!v)}
              />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="Engine" icon={<Settings2 size={15} />}>
            <PropertyGroup>
              <PropertyDropdown
                label="Frame rate cap"
                description="Off = display refresh. Applied at engine start."
                value={String(Number(maxFps) || 0)}
                options={FPS_OPTIONS}
                onChange={(v) => setMaxFps(Number(v) || 0)}
              />
              <PropertyToggle
                label="Hardware video decoding"
                description="GPU default. Off = CPU decode. Applied at engine start."
                checked={hwDecode !== false}
                onChange={(v) => setHwDecode(v)}
              />
              <PropertyToggle
                label="Leave the last frame on exit"
                description="Freeze the final frame as a still. No black desktop."
                checked={snapshotOnQuit !== false}
                onChange={(v) => setSnapshotOnQuit(v)}
              />
              <PropertyToggle
                label="Play audio only when the desktop is clear"
                description="Mute when covered. Unmute when clear."
                checked={audioOnlyWhenClosed === true}
                onChange={(v) => setAudioOnlyWhenClosed(v)}
              />
            </PropertyGroup>
            <PropertyGroup>
              <EngineStatus />
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="Advanced" icon={<Bug size={15} />} defaultOpen={false}>
            <PropertyGroup>
              <PropertyToggle
                label="Test mode diagnostics"
                description="Local-only engine log. Downloads\\Paiint Diagnostics. No upload."
                checked={diagnosticsEnabled === true}
                onChange={(v) => setDiagnosticsEnabled(v)}
              />
              <PropertyRow
                label="Diagnostics folder"
                description={
                  diagStatus?.enabled
                    ? `Recording${diagStatus.capReached ? ' (200 MB cap reached)' : ''} — ${diagStatus.samples} samples`
                    : diagStatus?.fallback
                      ? 'Downloads was not writable; recording to the app data folder'
                      : 'Off'
                }
              >
                <Button size="xs" variant="secondary" onClick={() => window.paiintDesktop?.diagnostics?.openFolder?.()}>
                  <FolderOpen size={13} /> Open
                </Button>
              </PropertyRow>
            </PropertyGroup>
          </PropertySection>

          <PropertySection title="New wallpapers" icon={<Palette size={15} />}>
            <PropertyGroup>
              <PropertyDropdown
                label="Default fit"
                description="Fit mode for new imports."
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
        </div>
      </div>
    </TooltipProvider>
  )
}
