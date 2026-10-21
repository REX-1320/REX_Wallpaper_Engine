import { useEffect, useRef, useState } from 'react'
import { MousePointerClick, Search, X } from 'lucide-react'
import { AnimatePresence, motion } from 'motion/react'
import { Button } from '@/components/ui/button'
import { ScrollArea } from '@/components/ui/scroll-area'
import { TooltipProvider } from '@/components/ui/tooltip'
import { InspectorSearchContext } from '@/components/properties/search'
import { selectWallpaper, useWallpaperStore } from '../store'
import { DESKTOP_CAPABILITIES } from '../desktop'
import { formatBytes, useMediaInfo } from '../mediaInfo'
import { getThumb } from '../thumbCache'
import PrimaryActions from './PrimaryActions'
import PlaybackSection from './sections/PlaybackSection'
import PictureSection from './sections/PictureSection'
import DisplaySection from './sections/DisplaySection'
import FileSection from './sections/FileSection'
import PlaylistSection from './sections/PlaylistSection'
import './wallpaper.css'

export default function WallpaperInspector() {
  const { wallpapers, selectedId, appliedId } = useWallpaperStore()
  const [query, setQuery] = useState('')
  const [searchOpen, setSearchOpen] = useState(false)
  const searchInputRef = useRef(null)
  const wallpaper = wallpapers.find((w) => w.id === selectedId) ?? null
  const applied = Boolean(wallpaper && appliedId === wallpaper.id)
  const capabilities = wallpaper ? DESKTOP_CAPABILITIES[wallpaper.type] ?? [] : []
  // Real file facts: the registry's defaults (resolution, size) are made up,
  // so the panel shows what the file actually is.
  const media = useMediaInfo(wallpaper?.filePath)
  const fileSize = formatBytes(media.size)
  const poster = wallpaper
    ? getThumb(wallpaper.filePath ?? wallpaper.info?.filePath ?? wallpaper.info?.source)?.dataUrl ?? null
    : null

  useEffect(() => {
    if (searchOpen) searchInputRef.current?.focus()
  }, [searchOpen])

  return (
    <TooltipProvider delayDuration={200}>
      <InspectorSearchContext.Provider value={query.toLowerCase().trim()}>
        <motion.aside
          className="flex min-h-0 w-full flex-1 flex-col overflow-hidden"
          initial={{ opacity: 0, y: 24 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ type: 'spring', stiffness: 260, damping: 26 }}
        >
          {wallpaper ? (
            <div className="insp-poster">
              {poster ? <img src={poster} alt="" /> : <MousePointerClick size={20} />}
            </div>
          ) : null}

          <div className="insp-head">
            {wallpaper ? (
              <>
                <div className="flex min-w-0 flex-1 flex-col">
                  <span className="insp-title">{wallpaper.info.name}</span>
                  <span className="insp-meta">
                    <span>{wallpaper.type}</span>
                    {fileSize ? <span>· {fileSize}</span> : null}
                  </span>
                </div>
                <Button
                  variant="ghost"
                  size="icon-xs"
                  aria-label="Deselect wallpaper"
                  onClick={() => selectWallpaper(null)}
                >
                  <X />
                </Button>
              </>
            ) : (
              <span className="insp-title">Wallpaper Inspector</span>
            )}
          </div>

          <div className="px-3 pt-2">
            <motion.div
              className={`search-pill ${searchOpen ? 'open' : ''}`}
              animate={{ width: searchOpen ? 236 : 38 }}
              transition={{ type: 'spring', stiffness: 260, damping: 28 }}
            >
              <button
                type="button"
                className="search-pill-icon"
                aria-label="Search properties"
                onClick={() => setSearchOpen((v) => !v)}
              >
                <Search size={16} />
              </button>
              <motion.input
                ref={searchInputRef}
                className="search-pill-input"
                placeholder="Search properties…"
                initial={false}
                animate={{
                  width: searchOpen ? 180 : 0,
                  opacity: searchOpen ? 1 : 0,
                }}
                transition={{ type: 'spring', stiffness: 260, damping: 28 }}
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Escape') setSearchOpen(false)
                }}
              />
            </motion.div>
          </div>

          <ScrollArea className="min-h-[180px] flex-1">
            <div className="flex flex-col gap-2.5 p-3">
              <AnimatePresence mode="wait">
                {wallpaper ? (
                  <motion.div
                    key={wallpaper.id}
                    initial={{ opacity: 0, y: 6 }}
                    animate={{ opacity: 1, y: 0 }}
                    exit={{ opacity: 0, y: -6 }}
                    transition={{ duration: 0.18 }}
                    className="flex flex-col gap-2.5"
                  >
                    <PrimaryActions wallpaper={wallpaper} applied={applied} />
                    <PlaybackSection
                      wallpaper={wallpaper}
                      applied={applied}
                      capabilities={capabilities}
                      media={media}
                    />
                    <PictureSection
                      wallpaper={wallpaper}
                      applied={applied}
                      capabilities={capabilities}
                    />
                    <DisplaySection wallpaper={wallpaper} applied={applied} />
                    <FileSection wallpaper={wallpaper} />
                    <PlaylistSection wallpaper={wallpaper} />
                  </motion.div>
                ) : (
                  <motion.div key="empty" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
                    <div className="insp-empty">
                      <span className="insp-empty-icon">
                        <MousePointerClick size={22} />
                      </span>
                      <div>
                        <div className="insp-empty-title">No wallpaper selected</div>
                        <div className="insp-empty-hint">
                          Pick one in the gallery to edit how it plays.
                        </div>
                      </div>
                    </div>
                  </motion.div>
                )}
              </AnimatePresence>
            </div>
          </ScrollArea>
        </motion.aside>
      </InspectorSearchContext.Provider>
    </TooltipProvider>
  )
}
