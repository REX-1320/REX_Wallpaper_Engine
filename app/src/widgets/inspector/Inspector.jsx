import { useState } from 'react'
import { MousePointerClick, Search, X } from 'lucide-react'
import { AnimatePresence, motion } from 'motion/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Empty, EmptyContent, EmptyDescription, EmptyMedia, EmptyTitle } from '@/components/ui/empty'
import { Input } from '@/components/ui/input'
import { ScrollArea } from '@/components/ui/scroll-area'
import { TooltipProvider } from '@/components/ui/tooltip'
import { InspectorSearchContext } from '@/components/properties/search'
import { selectWidget, useWidgetStore } from '../store'
import PositionSection from './sections/PositionSection'
import BehaviorSection from './sections/BehaviorSection'
import AppearanceSection from './sections/AppearanceSection'
import LayerSection from './sections/LayerSection'
import InteractionSection from './sections/InteractionSection'
import InformationSection from './sections/InformationSection'
import ActionsSection from './sections/ActionsSection'

export default function WidgetInspector() {
  const { widgets, selectedId } = useWidgetStore()
  const [query, setQuery] = useState('')
  const widget = widgets.find((w) => w.id === selectedId) ?? null

  return (
    <TooltipProvider delayDuration={200}>
      <InspectorSearchContext.Provider value={query.toLowerCase().trim()}>
        <motion.aside
          className="flex h-[300px] w-full shrink-0 flex-col overflow-hidden rounded-xl border border-border/60 bg-card/40 backdrop-blur-2xl"
          initial={{ opacity: 0, y: 24 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ type: 'spring', stiffness: 260, damping: 26 }}
        >
          <div className="flex items-center justify-between gap-2 border-b border-border/60 px-3 py-2.5">
            {widget ? (
              <>
                <div className="flex min-w-0 items-center gap-2">
                  <span className="truncate text-sm font-semibold text-foreground">{widget.name}</span>
                  <Badge variant="secondary" className="shrink-0 text-[9px] tracking-wider uppercase">
                    {widget.category}
                  </Badge>
                </div>
                <Button
                  variant="ghost"
                  size="icon-xs"
                  aria-label="Deselect widget"
                  onClick={() => selectWidget(null)}
                >
                  <X />
                </Button>
              </>
            ) : (
              <span className="text-sm font-semibold text-foreground">Inspector</span>
            )}
          </div>

          <div className="px-3 pt-2">
            <div className="relative">
              <Search className="absolute top-1/2 left-2.5 size-3.5 -translate-y-1/2 text-muted-foreground" />
              <Input
                type="search"
                placeholder="Search properties…"
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                className="h-8 pl-8 text-xs"
              />
            </div>
          </div>

          <ScrollArea className="min-h-0 flex-1">
            <div className="flex flex-col gap-2.5 p-3">
              <AnimatePresence mode="wait">
                {widget ? (
                  <motion.div
                    key={widget.id}
                    initial={{ opacity: 0, y: 6 }}
                    animate={{ opacity: 1, y: 0 }}
                    exit={{ opacity: 0, y: -6 }}
                    transition={{ duration: 0.18 }}
                    className="flex flex-col gap-2.5"
                  >
                    <PositionSection widget={widget} />
                    <BehaviorSection widget={widget} />
                    <AppearanceSection widget={widget} />
                    <LayerSection widget={widget} />
                    <InteractionSection widget={widget} />
                    <InformationSection widget={widget} />
                    <ActionsSection widget={widget} />
                  </motion.div>
                ) : (
                  <motion.div
                    key="empty"
                    initial={{ opacity: 0 }}
                    animate={{ opacity: 1 }}
                    exit={{ opacity: 0 }}
                  >
                    <Empty className="min-h-52">
                      <EmptyMedia variant="icon">
                        <MousePointerClick className="size-5" />
                      </EmptyMedia>
                      <EmptyContent>
                        <EmptyTitle>No widget selected</EmptyTitle>
                        <EmptyDescription>
                          Select a widget on the desktop preview to edit its properties.
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
