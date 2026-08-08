import { useEffect, useRef, useState } from 'react'
import { MousePointerClick, Search, X } from 'lucide-react'
import { AnimatePresence, motion } from 'motion/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Empty, EmptyContent, EmptyDescription, EmptyMedia, EmptyTitle } from '@/components/ui/empty'
import { ScrollArea } from '@/components/ui/scroll-area'
import { TooltipProvider } from '@/components/ui/tooltip'
import { InspectorSearchContext } from '@/components/properties/search'
import { selectWallpaper, useWallpaperStore } from '../store'
import AppearanceSection from './sections/AppearanceSection'
import RendererSection from './sections/RendererSection'
import ActionsSection from './sections/ActionsSection'

export default function WallpaperInspector() {
  const { wallpapers, selectedId } = useWallpaperStore()
  const [query, setQuery] = useState('')
  const [searchOpen, setSearchOpen] = useState(false)
  const searchInputRef = useRef(null)
  const wallpaper = wallpapers.find((w) => w.id === selectedId) ?? null

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
          <div className="flex items-center justify-between gap-2 border-b border-border/60 px-3 py-2.5">
            {wallpaper ? (
              <>
                <div className="flex min-w-0 items-center gap-2">
                  <span className="truncate text-sm font-semibold text-foreground">
                    {wallpaper.info.name}
                  </span>
                  <Badge variant="secondary" className="shrink-0 text-[9px] tracking-wider uppercase">
                    {wallpaper.type}
                  </Badge>
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
              <span className="text-sm font-semibold text-foreground">Wallpaper Inspector</span>
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

          <ScrollArea className="min-h-0 flex-1">
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
                    <AppearanceSection wallpaper={wallpaper} />
                    <RendererSection wallpaper={wallpaper} />
                    <ActionsSection wallpaper={wallpaper} />
                  </motion.div>
                ) : (
                  <motion.div key="empty" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
                    <Empty className="min-h-52">
                      <EmptyMedia variant="icon">
                        <MousePointerClick className="size-5" />
                      </EmptyMedia>
                      <EmptyContent>
                        <EmptyTitle>No wallpaper selected</EmptyTitle>
                        <EmptyDescription>
                          Select a wallpaper on the preview to edit its properties.
                        </EmptyDescription>
                      </EmptyContent>
                    </Empty>
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
