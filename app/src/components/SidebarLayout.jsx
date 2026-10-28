import { useState, useRef, useEffect } from 'react'
import { motion } from 'motion/react'
import './SidebarLayout.css'
import { ChevronLeft, ChevronRight, Minus, Square, X, Search, Image, Settings, Info, User } from 'lucide-react'
import Glass_effect from './Glass_effect'
import paiintMark from '../assets/paiint-mark.png'
import DesktopApply from '../wallpapers/DesktopApply'
import PlaylistDock from '../wallpapers/PlaylistDock'
import { setLayoutState, useLayoutState } from '../state/layout'

// The nav column's geometry lives here, in one place: the sidebar's animated
// width and the content column's offset are derived from the same numbers,
// so the gap between them can never drift.
const SIDEBAR_W = 240
const SIDEBAR_CLOSED_W = 51
const CONTENT_GAP = 20
const DOCK_MIN_HEIGHT = 120
const DOCK_MAX_RATIO = 0.7

const NAV_GROUPS = [
  {
    label: 'Desktop',
    items: [
      { icon: Image, label: 'Wallpapers' },
    ],
  },
  {
    label: 'Advanced',
    items: [
      { icon: Settings, label: 'Settings' },
      { icon: Info, label: 'About' },
    ],
  },
]

function SidebarLayout({ active, onSelect, children }) {
  const [open, setOpen] = useState(true)
  const [searchOpen, setSearchOpen] = useState(false)
  const [maximized, setMaximized] = useState(false)
  const searchInputRef = useRef(null)
  const layout = useLayoutState()
  const [liveHeight, setLiveHeight] = useState(null)
  const dragStartRef = useRef(null)
  const liveHeightRef = useRef(null)

  useEffect(() => {
    if (searchOpen) searchInputRef.current?.focus()
  }, [searchOpen])

  // Custom title bar state — driven by the Electron main process.
  // Fully optional-chained: safe in browser/dev where the bridge is absent.
  useEffect(() => {
    try {
      window.paiintDesktop?.isMaximized?.()?.then?.(setMaximized)?.catch?.(() => {})
      return window.paiintDesktop?.onMaximized?.(setMaximized)
    } catch {
      return undefined
    }
  }, [])

  // Tray-driven navigation (e.g. tray "Settings" -> Settings tab).
  useEffect(() => {
    return window.paiintDesktop?.onNavigate?.((page) => onSelect?.(page))
  }, [onSelect])

  const winControls = {
    minimize: () => window.paiintDesktop?.minimize?.(),
    toggleMaximize: () => window.paiintDesktop?.toggleMaximize?.(),
    close: () => window.paiintDesktop?.close?.(),
  }

  // The titlebar strip is a real `-webkit-app-region: drag` region and the window
  // is not transparent, so Windows owns move / double-click maximize / Aero Snap
  // through its own caption (HTCAPTION) loop — identical to Windows Terminal.
  // That means the strip swallows DOM mouse events by design; anything that must
  // stay clickable carries `-webkit-app-region: no-drag` in the CSS instead
  // (traffic-light dots, search pill, sidebar toggle).

  const effectiveHeight = liveHeight ?? layout.panelHeight

  const onDockResize = (delta) => {
    if (dragStartRef.current == null) dragStartRef.current = effectiveHeight
    const max = Math.round(window.innerHeight * DOCK_MAX_RATIO)
    const next = Math.min(Math.max(DOCK_MIN_HEIGHT, dragStartRef.current - delta), max)
    liveHeightRef.current = next
    setLiveHeight(next)
  }

  const onDockCommit = () => {
    // Only the committed value is persisted — dragging writes on every frame
    // otherwise, and the state file is flushed to disk.
    if (liveHeightRef.current != null) setLayoutState({ panelHeight: liveHeightRef.current })
    dragStartRef.current = null
    liveHeightRef.current = null
    setLiveHeight(null)
  }

  return (
    <div className="window">
      <DesktopApply />
      <Glass_effect dark />
      <div className="titlebar-drag" />

      <motion.div
        className="titlebar"
        initial={{ y: -60, opacity: 0 }}
        animate={{ y: 0, opacity: 1 }}
        transition={{ type: 'spring', stiffness: 200, damping: 20 }}
      >
        <div className="titlebar-controls">
          <button
            type="button"
            className="titlebar-dot yellow"
            aria-label="Minimize"
            onClick={winControls.minimize}
          >
            <Minus size={13} />
          </button>
          <button
            type="button"
            className={`titlebar-dot green${maximized ? ' maximized' : ''}`}
            aria-label={maximized ? 'Restore' : 'Maximize'}
            onClick={winControls.toggleMaximize}
          >
            <Square size={11} />
          </button>
          <button type="button" className="titlebar-dot red" aria-label="Close" onClick={winControls.close}>
            <X size={13} />
          </button>
        </div>

        <div className="titlebar-icons">
          <motion.div
            className={`search-pill ${searchOpen ? 'open' : ''}`}
            animate={{ width: searchOpen ? 236 : 38 }}
            transition={{ type: 'spring', stiffness: 260, damping: 28 }}
          >
            <button
              type="button"
              className="search-pill-icon"
              aria-label="Search"
              onClick={() => setSearchOpen(!searchOpen)}
            >
              <Search size={18} />
            </button>
            <motion.input
              ref={searchInputRef}
              className="search-pill-input"
              placeholder="Search…"
              initial={false}
              animate={{
                width: searchOpen ? 180 : 0,
                opacity: searchOpen ? 1 : 0,
              }}
              transition={{ type: 'spring', stiffness: 260, damping: 28 }}
              onKeyDown={(e) => {
                if (e.key === 'Escape') setSearchOpen(false)
              }}
            />
          </motion.div>
          <button type="button" className="titlebar-icon" aria-label="Profile">
            <User size={18} />
          </button>
        </div>
      </motion.div>

      <h3 className="sidebar-heading">
        <img className="sidebar-logo" src={paiintMark} alt="Paiint" draggable={false} />
      </h3>

      <motion.aside
        className={`sidebar ${open ? '' : 'closed'}`}
        initial={{ x: -280, opacity: 0 }}
        animate={{
          x: 0,
          opacity: 1,
          width: open ? SIDEBAR_W : SIDEBAR_CLOSED_W,
        }}
        transition={{ type: 'spring', stiffness: 180, damping: 24 }}
      >
        <button
          type="button"
          className="sidebar-toggle"
          onClick={() => setOpen(!open)}
          aria-label={open ? 'Collapse sidebar' : 'Expand sidebar'}
        >
          {open ? <ChevronLeft size={18} /> : <ChevronRight size={18} />}
        </button>

        <nav className="sidebar-nav">
          {NAV_GROUPS.map((group, gi) => (
            <div key={group.label} className="sidebar-group">
              <h4 className="sidebar-group-title">{group.label}</h4>
              {group.items.map(({ icon: Icon, label }, i) => (
                <motion.a
                  key={label}
                  href="#"
                  onClick={(e) => {
                    e.preventDefault()
                    onSelect(label)
                  }}
                  className={`sidebar-link ${active === label ? 'active' : ''}`}
                  initial={{ x: -20, opacity: 0 }}
                  animate={{ x: 0, opacity: 1 }}
                  transition={{ delay: 0.15 + (gi * 4 + i) * 0.06, type: 'spring', stiffness: 200, damping: 20 }}
                  whileTap={{ scale: 0.98 }}
                  title={label}
                >
                  <Icon size={18} />
                  <span className="sidebar-label">{label}</span>
                </motion.a>
              ))}
            </div>
          ))}
        </nav>
      </motion.aside>

      <motion.main
        className={`content-col${layout.panelMaximized ? ' dock-maximized' : ''}`}
        initial={{ left: SIDEBAR_W + CONTENT_GAP }}
        animate={{ left: open ? SIDEBAR_W + CONTENT_GAP : SIDEBAR_CLOSED_W + CONTENT_GAP }}
        transition={{ type: 'spring', stiffness: 180, damping: 24 }}
      >
        <div className="page-area">{children}</div>
        {/* The playlist dock belongs to the Wallpapers page — the playlists are
            wallpapers' playlists, so it does not follow the user onto
            Settings/About. Its size and state are remembered either way. */}
        {active === 'Wallpapers' ? (
          <PlaylistDock
            height={effectiveHeight}
            open={layout.panelOpen}
            minimized={layout.panelMinimized}
            maximized={layout.panelMaximized}
            onResize={onDockResize}
            onCommitResize={onDockCommit}
            onMinimize={() =>
              setLayoutState({ panelMinimized: !layout.panelMinimized, panelMaximized: false })
            }
            onMaximize={() =>
              setLayoutState({ panelMaximized: !layout.panelMaximized, panelMinimized: false })
            }
            onClose={() => setLayoutState({ panelOpen: false })}
          />
        ) : null}
      </motion.main>
    </div>
  )
}

export default SidebarLayout
