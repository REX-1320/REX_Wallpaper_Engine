import { useEffect, useRef, useState } from 'react'
import { motion } from 'motion/react'
import SidebarLayout from './components/SidebarLayout'
import ResizeHandle from './components/ResizeHandle'
import WallpaperGallery from './wallpapers/WallpaperGallery'
import WallpaperInspector from './wallpapers/inspector/Inspector'
import SettingsPage from './settings/SettingsPage'
import AboutPage from './about/AboutPage'
import { Toaster } from './components/ui/sonner'
import { initLayoutState, setLayoutState, useLayoutState } from './state/layout'
import { initLastAppliedState, initPlaylistState } from './wallpapers/store'

const GALLERY_MIN = 280
const INSPECTOR_MIN = 320
// The handle's column replaces the panels' old 18px gutter.
const HANDLE_WIDTH = 18
// The default split, as a share of the panels' width (the rest is the
// inspector). Measured from the layout as it stood on 2026-10-20:
// gallery 1723px / inspector 511px = 77.1% / 22.9%. A ratio — not pixels — so
// the default looks the same in any window.
const DEFAULT_GALLERY_RATIO = 0.771

const PAGES = {
  Wallpapers: {
    sub: 'Library. Playlists. Desktop.',
  },
  Settings: {
    sub: 'Runtime configuration.',
  },
  About: {
    sub: 'The build, the engine, the licences.',
  },
}

function App() {
  const [tab, setTab] = useState('Wallpapers')
  const layout = useLayoutState()
  const gridRef = useRef(null)
  const [liveSplit, setLiveSplit] = useState(null)
  const [gridWidth, setGridWidth] = useState(0)
  const splitStartRef = useRef(null)
  const liveSplitRef = useRef(null)

  useEffect(() => {
    initLayoutState()
    initPlaylistState()
    initLastAppliedState()
  }, [])

  // The split is persisted in pixels, but the window can be narrower than the
  // saved width — the columns are clamped to what actually fits, so the panels
  // can never overflow the window.
  useEffect(() => {
    const el = gridRef.current
    if (!el) return undefined
    const measure = () => setGridWidth(el.clientWidth)
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [tab])

  const maxSplit = Math.max(GALLERY_MIN, gridWidth - INSPECTOR_MIN - HANDLE_WIDTH)
  const rawSplit = liveSplit ?? layout.gallerySplitPx
  const split =
    rawSplit == null
      ? Math.min(Math.round((gridWidth - HANDLE_WIDTH) * DEFAULT_GALLERY_RATIO), maxSplit)
      : Math.min(rawSplit, maxSplit)

  const onSplitResize = (delta) => {
    if (splitStartRef.current == null) {
      const first = gridRef.current?.firstElementChild?.getBoundingClientRect().width
      splitStartRef.current = first && first > 0 ? first : (window.innerWidth - 260) / 2
    }
    const gridWidthNow = gridRef.current?.getBoundingClientRect().width ?? window.innerWidth
    const max = Math.max(GALLERY_MIN, gridWidthNow - INSPECTOR_MIN - HANDLE_WIDTH)
    const next = Math.min(Math.max(GALLERY_MIN, splitStartRef.current + delta), max)
    liveSplitRef.current = next
    setLiveSplit(next)
  }

  const onSplitCommit = () => {
    // The width is persisted on commit only — the state file is flushed to
    // disk, and a drag would otherwise write on every pointer move.
    if (liveSplitRef.current != null) setLayoutState({ gallerySplitPx: liveSplitRef.current })
    splitStartRef.current = null
    liveSplitRef.current = null
    setLiveSplit(null)
  }

  const gridStyle = split
    ? { gridTemplateColumns: `${split}px ${HANDLE_WIDTH}px minmax(${INSPECTOR_MIN}px, 1fr)` }
    : undefined

  return (
    <SidebarLayout active={tab} onSelect={setTab}>
      <motion.section
        className="content-section"
        key={tab}
        initial={{ opacity: 0, y: 16 }}
        animate={{ opacity: 1, y: 0 }}
        transition={{ type: 'spring', stiffness: 260, damping: 24 }}
      >
        <header className="page-head">
          <div className="page-head-text">
            <h2>{tab}</h2>
            {PAGES[tab]?.sub ? <p className="page-sub">{PAGES[tab].sub}</p> : null}
          </div>
          <div className="page-head-rule" />
        </header>
        {tab === 'Wallpapers' ? (
          <div className="tab-panel plain">
            <div className="wallpaper-grid" ref={gridRef} style={gridStyle}>
              <div className="wallpaper-cube w1">
                <WallpaperGallery />
              </div>
              <ResizeHandle
                axis="x"
                className="split-handle"
                label="Resize gallery"
                onResize={onSplitResize}
                onCommit={onSplitCommit}
              />
              <div className="wallpaper-cube w2">
                <WallpaperInspector />
              </div>
            </div>
          </div>
        ) : null}
        {tab === 'Settings' ? (
          <div className="tab-panel plain">
            <SettingsPage />
          </div>
        ) : null}
        {tab === 'About' ? (
          <div className="tab-panel plain">
            <AboutPage />
          </div>
        ) : null}
      </motion.section>
      {/* User-facing error toasts (P1): mounted at the root so every tab
          can report wallpaper failures, not just the demo tab. */}
      <Toaster position="top-right" richColors />
    </SidebarLayout>
  )
}

export default App
