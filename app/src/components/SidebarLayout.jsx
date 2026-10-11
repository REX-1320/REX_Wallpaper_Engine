import { useState, useRef, useEffect } from 'react'
import { motion } from 'motion/react'
import './SidebarLayout.css'
import { Home, Component, Settings, User, Bell, ChevronLeft, ChevronRight, Minus, Square, X, Blocks, Search, Image, Palette, LayoutGrid, Paintbrush, Store, Download, Star, Gauge, Package, Activity, FlaskConical, Info } from 'lucide-react'
import Glass_effect from './Glass_effect'
import ShinyText from './ShinyText'
import DesktopApply from '../wallpapers/DesktopApply'

const NAV_GROUPS = [
  {
    label: 'Menu',
    items: [
      { icon: Home, label: 'Home' },
      { icon: Component, label: 'Buttons' },
      { icon: Settings, label: 'Settings' },
      { icon: User, label: 'Profile' },
      { icon: Bell, label: 'Alerts' },
      { icon: Blocks, label: 'shadcn' },
    ],
  },
  {
    label: 'Desktop',
    items: [
      { icon: Image, label: 'Wallpapers' },
      { icon: Palette, label: 'Themes' },
      { icon: LayoutGrid, label: 'Widgets' },
      { icon: Paintbrush, label: 'Personalization' },
    ],
  },
  {
    label: 'Discover',
    items: [
      { icon: Store, label: 'Marketplace' },
      { icon: Download, label: 'Downloads' },
      { icon: Star, label: 'Collections' },
    ],
  },
  {
    label: 'System',
    items: [
      { icon: Gauge, label: 'Performance' },
      { icon: Package, label: 'Modules' },
      { icon: Activity, label: 'Activity' },
      { icon: Settings, label: 'Settings' },
    ],
  },
  {
    label: 'Advanced',
    items: [
      { icon: FlaskConical, label: 'Developer' },
      { icon: Info, label: 'About' },
    ],
  },
]

function SidebarLayout({ active, onSelect, children }) {
  const [open, setOpen] = useState(true)
  const [searchOpen, setSearchOpen] = useState(false)
  const [maximized, setMaximized] = useState(false)
  const searchInputRef = useRef(null)

  useEffect(() => {
    if (searchOpen) searchInputRef.current?.focus()
  }, [searchOpen])

  // Custom title bar state — driven by the Electron main process.
  // Fully optional-chained: safe in browser/dev where the bridge is absent.
  useEffect(() => {
    try {
      window.rexDesktop?.isMaximized?.()?.then?.(setMaximized)?.catch?.(() => {})
      return window.rexDesktop?.onMaximized?.(setMaximized)
    } catch {
      return undefined
    }
  }, [])

  // Tray-driven navigation (e.g. tray "Settings" -> Settings tab).
  useEffect(() => {
    return window.rexDesktop?.onNavigate?.((page) => onSelect?.(page))
  }, [onSelect])

  const winControls = {
    minimize: () => window.rexDesktop?.minimize?.(),
    toggleMaximize: () => window.rexDesktop?.toggleMaximize?.(),
    close: () => window.rexDesktop?.close?.(),
  }

  // The titlebar strip is a real `-webkit-app-region: drag` region and the window
  // is not transparent, so Windows owns move / double-click maximize / Aero Snap
  // through its own caption (HTCAPTION) loop — identical to Windows Terminal.
  // That means the strip swallows DOM mouse events by design; anything that must
  // stay clickable carries `-webkit-app-region: no-drag` in the CSS instead
  // (traffic-light dots, search pill, sidebar toggle).

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
            className={`titlebar-dot green${maximized ? ' maximized' : ''}`}
            aria-label={maximized ? 'Restore' : 'Maximize'}
            onClick={winControls.toggleMaximize}
          >
            <Square size={11} />
          </button>
          <button type="button" className="titlebar-dot yellow" aria-label="Minimize" onClick={winControls.minimize}>
            <Minus size={13} />
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
        <ShinyText text="Y0!" color="#8f8f8f" shineColor="#ffffff" speed={3} />
      </h3>

      <motion.aside
        className={`sidebar ${open ? '' : 'closed'}`}
        initial={{ x: -280, opacity: 0 }}
        animate={{
          x: 0,
          opacity: 1,
          width: open ? 240 : 51,
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
              {open && <h4 className="sidebar-group-title">{group.label}</h4>}
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
                  whileHover={{ scale: 1.03 }}
                  whileTap={{ scale: 0.97 }}
                  title={label}
                >
                  <Icon size={18} />
                  {open && <span className="sidebar-label">{label}</span>}
                </motion.a>
              ))}
            </div>
          ))}
        </nav>
      </motion.aside>

      <motion.main
        className="window-content"
        animate={{ left: open ? 260 : 71 }}
        transition={{ type: 'spring', stiffness: 180, damping: 24 }}
      >
        {children}
      </motion.main>
    </div>
  )
}

export default SidebarLayout
