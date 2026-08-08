import { useEffect, useRef, useState } from 'react'
import {
  Activity,
  BarChart3,
  Bookmark,
  ChevronDown,
  Filter,
  Folders,
  History as HistoryIcon,
  ListMusic,
  ListOrdered,
  Monitor,
  Power,
  Settings2,
  Shield,
  SlidersHorizontal,
  Timer as TimerIcon,
  Wand2,
  X,
  Zap,
} from 'lucide-react'
import { Input } from '@/components/ui/input'
import PropertyRow from '@/components/properties/PropertyRow'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyNumber from '@/components/properties/PropertyNumber'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertyButton from '@/components/properties/PropertyButton'
import PropertyText from '@/components/properties/PropertyText'
import { useWallpaperStore } from '../store'
import { PLAYLIST_ICONS } from '../registry'
import {
  FILTER_INCLUDES,
  MULTIMONITOR_OPTIONS,
  PLAYBACK_ORDERS,
  TRANSITION_CURVES,
  usePlaylistEngine,
} from './engine'

const SIMPLE_MODES = [
  { value: 'Manual', label: 'Manual' },
  { value: 'Timer', label: 'Timer' },
  { value: 'After Current Ends', label: 'When Current Ends' },
]
const ADVANCED_MODES = [
  'Manual',
  'Timer',
  'After Current Ends',
  'Video Ends',
  'Daily Schedule',
  'Weekly Schedule',
  'Idle Time',
  'Battery Level',
  'Weather',
  'Random Interval',
]
const SIMPLE_TRANSITIONS = ['None', 'Fade', 'Slide', 'Zoom', 'Blur', 'Random']
const WEATHERS = ['Clear', 'Rainy', 'Snowy', 'Cloudy', 'Stormy', 'Sunny', 'Foggy', 'Windy']
const WEEK_DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday']
const PREVIEW_ANIM = {
  Fade: 'pl-a-fade',
  Slide: 'pl-a-slide',
  Zoom: 'pl-a-zoom',
  Blur: 'pl-a-blur',
  Random: 'pl-a-spiral',
  None: 'pl-a-none',
}

function fmtClock(ms) {
  const total = Math.max(0, Math.floor(ms / 1000))
  const d = Math.floor(total / 86400)
  const h = Math.floor((total % 86400) / 3600)
  const m = Math.floor((total % 3600) / 60)
  const s = total % 60
  const pad = (n) => String(n).padStart(2, '0')
  return `${d}d ${pad(h)}:${pad(m)}:${pad(s)}`
}

function TimeField({ label, value, onChange }) {
  return (
    <PropertyRow label={label}>
      <Input type="time" className="h-7 w-28 text-xs" value={value} onChange={(e) => onChange(e.target.value)} />
    </PropertyRow>
  )
}

function Section({ title, icon, children }) {
  return (
    <div className="pl-sec">
      <div className="pl-sec-title">
        {icon}
        <span>{title}</span>
      </div>
      <div className="pl-sec-body">{children}</div>
    </div>
  )
}

export default function PlaylistEngineDialog({ onClose, initialPlaylistId = null }) {
  const { wallpapers, playlists, selectedId } = useWallpaperStore()
  const { engine, settingsByPlaylist, presets, history, now, system, weather } = usePlaylistEngine()
  const [playlistId, setPlaylistId] = useState(
    () =>
      initialPlaylistId ??
      (playlists.find((p) => p.name === wallpapers.find((w) => w.id === selectedId)?.playlist?.currentPlaylist) ?? playlists[0])?.id ??
      null,
  )
  const [advancedOpen, setAdvancedOpen] = useState(false)
  const [previewKey, setPreviewKey] = useState(0)
  const [presetName, setPresetName] = useState('')
  const [newName, setNewName] = useState('')
  const [renameName, setRenameName] = useState('')
  const [mergeWith, setMergeWith] = useState('')
  const importPlaylistRef = useRef(null)
  const importPresetRef = useRef(null)
  const restoreRef = useRef(null)

  useEffect(() => {
    const onKeyDown = (e) => {
      if (e.key === 'Escape') onClose()
    }
    document.addEventListener('keydown', onKeyDown)
    return () => document.removeEventListener('keydown', onKeyDown)
  }, [onClose])

  const playlist = playlists.find((p) => p.id === playlistId) ?? null
  const settings = playlistId ? settingsByPlaylist[playlistId] ?? engine.getSettings(playlistId) : null
  const selected = wallpapers.find((w) => w.id === selectedId) ?? null

  const set = (path, value) => engine.set(playlistId, path, value)
  if (!settings) return null

  const simpleMode = settings.mode
  const modeOptions = SIMPLE_MODES.some((m) => m.value === settings.mode)
    ? SIMPLE_MODES
    : [...SIMPLE_MODES, { value: settings.mode, label: settings.mode }]

  const simpleTransition = settings.transitionRandom ? 'Random' : settings.transition
  const durationSeconds = Math.round((settings.transitionDuration / 1000) * 10) / 10

  const pool = wallpapers.filter((w) => w.playlist.currentPlaylist === playlist?.name)
  const mostPlayed = (() => {
    const entries = Object.entries(history.mostPlayed).sort((a, b) => b[1] - a[1])
    return entries.length > 0 ? (wallpapers.find((w) => w.id === entries[0][0])?.info?.name ?? '—') : '—'
  })()
  const nextTs = engine.nextChangeTimestamp(playlistId)

  const readFile = (e, handler) => {
    const file = e.target.files?.[0]
    if (!file) return
    file.text().then((text) => {
      try {
        handler(text)
      } catch (err) {
        console.error('Playlist engine import failed', err)
      }
    })
    e.target.value = ''
  }

  const nameOf = (id) => wallpapers.find((w) => w.id === id)?.info?.name ?? id

  return (
    <div className="pl-dialog-overlay" onPointerDown={onClose}>
      <div className="pl-dialog pl-dialog-simple" role="dialog" aria-modal="true" onPointerDown={(e) => e.stopPropagation()}>
        <div className="pl-dialog-head">
          <span className="pl-dialog-title">
            <Settings2 size={14} />
            <span>Playlist Settings</span>
          </span>
          <div className="pl-dialog-select">
            <PropertyDropdown
              label=""
              value={playlistId ?? ''}
              options={playlists.map((p) => ({ value: p.id, label: p.name }))}
              onChange={(id) => setPlaylistId(id)}
            />
          </div>
          <button type="button" className="pl-dialog-close" aria-label="Close playlist settings" onClick={onClose}>
            <X size={14} />
          </button>
        </div>

        <div className="pl-dialog-body">
          <Section title="General" icon={<Settings2 size={12} />}>
            <PropertyDropdown
              label="Change Wallpaper"
              value={simpleMode}
              options={modeOptions}
              onChange={(v) => set(['mode'], v)}
            />
            {settings.mode === 'Timer' ? (
              <>
                <PropertyNumber label="Hours" value={settings.modeSettings.timer.hours} onChange={(v) => set(['modeSettings', 'timer', 'hours'], v)} min={0} max={23} />
                <PropertyNumber label="Minutes" value={settings.modeSettings.timer.minutes} onChange={(v) => set(['modeSettings', 'timer', 'minutes'], v)} min={0} max={59} />
              </>
            ) : null}
          </Section>

          <Section title="Order" icon={<ListOrdered size={12} />}>
            <PropertyDropdown
              label="Order"
              value={settings.order}
              options={['Sequential', 'Shuffle', 'Random'].includes(settings.order) ? ['Sequential', 'Shuffle', 'Random'] : [...['Sequential', 'Shuffle', 'Random'], settings.order]}
              onChange={(v) => set(['order'], v)}
            />
            <PropertyToggle label="Repeat Playlist" checked={settings.options.loopPlaylist} onChange={(v) => set(['options', 'loopPlaylist'], v)} />
            <PropertyToggle label="Repeat Current" checked={settings.options.loopCurrent} onChange={(v) => set(['options', 'loopCurrent'], v)} />
          </Section>

          <Section title="Transitions" icon={<Wand2 size={12} />}>
            <PropertyDropdown
              label="Transition"
              value={simpleTransition}
              options={SIMPLE_TRANSITIONS.includes(simpleTransition) ? SIMPLE_TRANSITIONS : [...SIMPLE_TRANSITIONS, simpleTransition]}
              onChange={(v) => {
                if (v === 'Random') set(['transitionRandom'], true)
                else {
                  set(['transitionRandom'], false)
                  set(['transition'], v)
                }
              }}
            />
            <PropertySlider
              label="Duration"
              value={durationSeconds}
              onChange={(v) => set(['transitionDuration'], Math.round(v * 1000))}
              min={0.2}
              max={5}
              step={0.1}
              suffix="s"
            />
            <PropertyGroup title="Preview">
              <div key={previewKey} className={`pl-preview ${PREVIEW_ANIM[simpleTransition] ?? 'pl-a-fade'}`} style={{ animationDuration: `${Math.max(400, settings.transitionDuration)}ms` }}>
                <span>{simpleTransition}</span>
              </div>
            </PropertyGroup>
          </Section>

          <Section title="Playback" icon={<Power size={12} />}>
            <PropertyToggle label="Change after video ends" checked={settings.options.changeAfterVideoEnds} onChange={(v) => set(['options', 'changeAfterVideoEnds'], v)} />
            <PropertyToggle label="Continue while paused" checked={settings.behavior.allowWhilePaused} onChange={(v) => set(['behavior', 'allowWhilePaused'], v)} />
            <PropertyToggle label="Resume previous wallpaper" checked={settings.startup.resumePrevious} onChange={(v) => set(['startup', 'resumePrevious'], v)} />
            <PropertyToggle label="Start from first wallpaper" checked={settings.startup.alwaysFirst} onChange={(v) => set(['startup', 'alwaysFirst'], v)} />
          </Section>

          <Section title="Actions" icon={<Activity size={12} />}>
            <div className="flex flex-col gap-1.5">
              <PropertyButton onClick={() => engine.manage.saveDefaults(playlistId)}>Save as Default</PropertyButton>
              <PropertyButton variant="secondary" onClick={() => engine.manage.resetSettings(playlistId)}>Reset</PropertyButton>
              <PropertyButton variant="secondary" onClick={() => setPreviewKey((k) => k + 1)}>Preview Transition</PropertyButton>
            </div>
          </Section>

          <div className="pl-advanced">
            <button type="button" className="pl-advanced-toggle" onClick={() => setAdvancedOpen((v) => !v)}>
              <SlidersHorizontal size={12} />
              <span>Advanced</span>
              <ChevronDown size={12} className={advancedOpen ? 'rot' : ''} />
            </button>
            {advancedOpen ? (
              <div className="pl-advanced-body">
                <Section title="Change Mode" icon={<TimerIcon size={12} />}>
                  <PropertyDropdown
                    label="Mode"
                    value={settings.mode}
                    options={ADVANCED_MODES.includes(settings.mode) ? ADVANCED_MODES : [...ADVANCED_MODES, settings.mode]}
                    onChange={(v) => set(['mode'], v)}
                  />
                  {(() => {
                    const ms = settings.modeSettings
                    switch (settings.mode) {
                      case 'Daily Schedule':
                        return <TimeField label="Time" value={ms.dailyTime} onChange={(v) => set(['modeSettings', 'dailyTime'], v)} />
                      case 'Weekly Schedule':
                        return (
                          <>
                            <PropertyDropdown label="Day" value={ms.weeklyDay} options={WEEK_DAYS} onChange={(v) => set(['modeSettings', 'weeklyDay'], v)} />
                            <TimeField label="Time" value={ms.weeklyTime} onChange={(v) => set(['modeSettings', 'weeklyTime'], v)} />
                          </>
                        )
                      case 'Idle Time':
                        return <PropertyNumber label="Idle Minutes" value={ms.idleMinutes} onChange={(v) => set(['modeSettings', 'idleMinutes'], v)} min={1} max={120} suffix="m" />
                      case 'Battery Level':
                        return <PropertySlider label="Trigger Below" value={ms.batteryLevel} onChange={(v) => set(['modeSettings', 'batteryLevel'], v)} min={5} max={95} suffix="%" />
                      case 'Weather':
                        return <PropertyDropdown label="Weather" value={ms.weatherTarget} options={WEATHERS} onChange={(v) => set(['modeSettings', 'weatherTarget'], v)} />
                      case 'Random Interval':
                        return (
                          <>
                            <PropertyNumber label="Min Seconds" value={ms.randomMin} onChange={(v) => set(['modeSettings', 'randomMin'], v)} min={1} max={86400} suffix="s" />
                            <PropertyNumber label="Max Seconds" value={ms.randomMax} onChange={(v) => set(['modeSettings', 'randomMax'], v)} min={1} max={86400} suffix="s" />
                          </>
                        )
                      default:
                        return null
                    }
                  })()}
                </Section>

                <Section title="Timer" icon={<TimerIcon size={12} />}>
                  <PropertyRow label="Countdown">
                    <span className="pl-mono pl-big">{fmtClock(engine.timerRemainingMs(settings))}</span>
                  </PropertyRow>
                  <div className="flex gap-1.5">
                    <PropertyButton onClick={() => engine.timerControl(playlistId, settings.modeSettings.timer.running ? 'pause' : 'resume')}>
                      {settings.modeSettings.timer.running ? 'Pause' : 'Resume'}
                    </PropertyButton>
                    <PropertyButton variant="secondary" onClick={() => engine.timerControl(playlistId, 'reset')}>
                      Reset
                    </PropertyButton>
                  </div>
                </Section>

                <Section title="Order & Transitions" icon={<Wand2 size={12} />}>
                  <PropertyDropdown label="Order" value={settings.order} options={PLAYBACK_ORDERS} onChange={(v) => set(['order'], v)} />
                  <PropertyToggle label="Random Transition" checked={settings.transitionRandom} onChange={(v) => set(['transitionRandom'], v)} />
                  <PropertyToggle label="Smart Transition" checked={settings.transitionSmart} onChange={(v) => set(['transitionSmart'], v)} />
                  <PropertyToggle label="Per-Wallpaper Transition" checked={settings.transitionPerWallpaper} onChange={(v) => set(['transitionPerWallpaper'], v)} />
                  <PropertyDropdown label="Curve" value={settings.transitionCurve} options={TRANSITION_CURVES} onChange={(v) => set(['transitionCurve'], v)} />
                </Section>

                <Section title="Playlist Options" icon={<Power size={12} />}>
                  {[
                    ['skipUnavailable', 'Skip unavailable wallpapers'],
                    ['skipUnsupportedRenderers', 'Skip unsupported renderers'],
                    ['autoRemoveMissing', 'Automatically remove missing files'],
                    ['validateBeforePlayback', 'Validate playlist before playback'],
                  ].map(([key, label]) => (
                    <PropertyToggle key={key} label={label} checked={settings.options[key]} onChange={(v) => set(['options', key], v)} />
                  ))}
                </Section>

                <Section title="Behavior" icon={<Activity size={12} />}>
                  {[
                    ['pauseOnBatterySaver', 'Pause on battery saver'],
                    ['pauseOnFullscreen', 'Pause when fullscreen application starts'],
                    ['pauseOnGame', 'Pause when game launches'],
                    ['continueAudioWhilePaused', 'Continue audio while paused'],
                  ].map(([key, label]) => (
                    <PropertyToggle key={key} label={label} checked={settings.behavior[key]} onChange={(v) => set(['behavior', key], v)} />
                  ))}
                </Section>

                <Section title="Multi Monitor" icon={<Monitor size={12} />}>
                  {Object.entries(MULTIMONITOR_OPTIONS).map(([key, label]) => (
                    <PropertyToggle key={key} label={label} checked={settings.multiMonitor[key]} onChange={(v) => set(['multiMonitor', key], v)} />
                  ))}
                </Section>

                <Section title="Filters & Rules" icon={<Filter size={12} />}>
                  <div className="pl-sec-title" style={{ marginTop: 4 }}>Include Types</div>
                  {Object.entries(FILTER_INCLUDES).map(([key, label]) => (
                    <PropertyToggle key={key} label={label} checked={settings.filters.include[key]} onChange={(v) => set(['filters', 'include', key], v)} />
                  ))}
                  <PropertySlider label="Min Rating" value={settings.filters.minRating} onChange={(v) => set(['filters', 'minRating'], v)} min={0} max={5} step={0.5} suffix="★" />
                  <PropertyText label="Tags" value={settings.filters.tags} onChange={(v) => set(['filters', 'tags'], v)} />
                  <PropertyToggle label="Favorites Only" checked={settings.filters.favorite} onChange={(v) => set(['filters', 'favorite'], v)} />
                  {[
                    ['skipNsfw', 'Skip NSFW'],
                    ['skipLargeFiles', 'Skip Large Files'],
                    ['only4k', 'Only 4K Wallpapers'],
                    ['onlyFavorites', 'Only Favorite Wallpapers'],
                  ].map(([key, label]) => (
                    <PropertyToggle key={key} label={label} checked={settings.rules[key]} onChange={(v) => set(['rules', key], v)} />
                  ))}
                </Section>

                <Section title="History" icon={<HistoryIcon size={12} />}>
                  <PropertyButton variant="secondary" onClick={() => engine.history.clear()}>Clear History</PropertyButton>
                  <div className="pl-list">
                    {history.recent.slice(0, 5).map((id, i) => (
                      <div key={`${id}-${i}`} className="pl-list-row">
                        <span>{nameOf(id)}</span>
                        <span className="pl-dim">{history.mostPlayed[id] ?? 0} plays</span>
                      </div>
                    ))}
                  </div>
                  <div className="pl-mono pl-dim">{history.skipped.length} skipped · {history.failed.length} failed · {history.transitionLog.length} transitions</div>
                </Section>

                <Section title="Statistics" icon={<BarChart3 size={12} />}>
                  <PropertyRow label="Total Wallpapers"><span className="pl-mono">{wallpapers.length}</span></PropertyRow>
                  <PropertyRow label="Playlist Size"><span className="pl-mono">{pool.length}</span></PropertyRow>
                  <PropertyRow label="Current"><span className="pl-mono pl-dim">{selected?.info?.name ?? '—'}</span></PropertyRow>
                  <PropertyRow label="Time Remaining"><span className="pl-mono">{nextTs ? fmtClock(Math.max(0, nextTs - now)) : '—'}</span></PropertyRow>
                  <PropertyRow label="Most Played"><span className="pl-mono pl-dim">{mostPlayed}</span></PropertyRow>
                  <PropertyRow label="CPU / GPU / RAM"><span className="pl-mono">{system.cpu}% / {system.gpu}% / {system.ram}%</span></PropertyRow>
                  <PropertyRow label="Battery"><span className="pl-mono">{system.battery === null ? '—' : `${system.battery}%${system.charging ? ' ⚡' : ''}`}</span></PropertyRow>
                  <PropertyRow label="Weather"><span className="pl-mono pl-dim">{weather}</span></PropertyRow>
                </Section>

                <Section title="Developer Options" icon={<Zap size={12} />}>
                  <PropertyRow label="Simulate Lock">
                    <div className="flex gap-1.5">
                      <PropertyButton onClick={() => engine.setSystem({ lockScreen: true })}>Lock</PropertyButton>
                      <PropertyButton onClick={() => engine.setSystem({ lockScreen: false })}>Unlock</PropertyButton>
                    </div>
                  </PropertyRow>
                  <PropertyDropdown label="Set Weather" value={weather} options={WEATHERS} onChange={(v) => engine.setWeather(v)} />
                  <PropertyNumber label="Set Temperature" value={system.temperature ?? 25} onChange={(v) => engine.setTemperature(v)} min={-40} max={120} suffix="°C" />
                </Section>

                <Section title="Presets" icon={<Bookmark size={12} />}>
                  <PropertyText label="Preset Name" value={presetName} onChange={setPresetName} />
                  <PropertyButton onClick={() => { if (presetName) { engine.presets.save(presetName); setPresetName('') } }}>Save Preset</PropertyButton>
                  <PropertyButton variant="secondary" onClick={() => importPresetRef.current?.click()}>Import Preset</PropertyButton>
                  <input ref={importPresetRef} type="file" hidden accept=".json" onChange={(e) => readFile(e, (text) => engine.presets.import(text))} />
                  {presets.map((preset) => (
                    <div key={preset.id} className="pl-list" style={{ gap: 6 }}>
                      <div className="pl-sec-title" style={{ marginTop: 4 }}>{preset.name}</div>
                      <div className="flex flex-col gap-1.5">
                        <PropertyButton onClick={() => engine.presets.apply(preset.id)}>Apply</PropertyButton>
                        <PropertyButton variant="secondary" onClick={() => engine.presets.duplicate(preset.id)}>Duplicate</PropertyButton>
                        <PropertyButton variant="secondary" onClick={() => engine.presets.export(preset.id)}>Export</PropertyButton>
                        <PropertyButton variant="danger" onClick={() => engine.presets.delete(preset.id)}>Delete</PropertyButton>
                      </div>
                    </div>
                  ))}
                </Section>

                <Section title="Manage Playlists" icon={<Folders size={12} />}>
                  <PropertyText label="New Playlist Name" value={newName} onChange={setNewName} />
                  <PropertyButton onClick={() => {
                    if (!newName) return
                    const newId = engine.manage.createPlaylist(newName, 'music', playlistId)
                    setPlaylistId(newId)
                    setNewName('')
                  }}>Create Playlist</PropertyButton>
                  {playlist ? (
                    <>
                      <div className="pl-sec-title" style={{ marginTop: 4 }}>
                        {(() => { const Icon = PLAYLIST_ICONS[playlist.icon] ?? ListMusic; return <Icon size={12} /> })()}
                        <span>{playlist.name}</span>
                      </div>
                      <PropertyText label="Rename To" value={renameName} onChange={setRenameName} />
                      <PropertyButton variant="secondary" onClick={() => { if (renameName) { engine.manage.renamePlaylist(playlistId, renameName); setRenameName('') } }}>Rename</PropertyButton>
                      <PropertyButton variant="secondary" onClick={() => engine.manage.duplicatePlaylist(playlistId)}>Duplicate</PropertyButton>
                      <PropertyButton variant="secondary" onClick={() => engine.manage.exportPlaylist(playlistId)}>Export</PropertyButton>
                      <PropertyButton variant="danger" onClick={() => engine.manage.deletePlaylist(playlistId)}>Delete</PropertyButton>
                      <PropertyButton variant="secondary" onClick={() => importPlaylistRef.current?.click()}>Import Playlist</PropertyButton>
                      <PropertyButton variant="secondary" onClick={() => engine.manage.backup()}>Backup All</PropertyButton>
                      <PropertyButton variant="secondary" onClick={() => restoreRef.current?.click()}>Restore Backup</PropertyButton>
                      <input ref={importPlaylistRef} type="file" hidden accept=".json" onChange={(e) => readFile(e, (text) => engine.manage.importPlaylist(text))} />
                      <input ref={restoreRef} type="file" hidden accept=".json" onChange={(e) => readFile(e, (text) => engine.manage.restore(text))} />
                    </>
                  ) : null}
                </Section>
              </div>
            ) : null}
          </div>
        </div>

        <div className="pl-dialog-foot">
          <button type="button" className="pl-dialog-ok" onClick={onClose}>
            OK
          </button>
        </div>
      </div>
    </div>
  )
}

function PropertyGroup({ title, children }) {
  return (
    <div className="pl-group">
      {title ? <div className="pl-sec-title">{title}</div> : null}
      <div className="pl-sec-body">{children}</div>
    </div>
  )
}
