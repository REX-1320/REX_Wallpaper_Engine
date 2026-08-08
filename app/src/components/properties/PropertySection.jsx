import { useState } from 'react'
import { ChevronDown } from 'lucide-react'
import { AnimatePresence, motion } from 'motion/react'
import { Collapsible, CollapsibleTrigger } from '@/components/ui/collapsible'
import { useInspectorSearch, matchesQuery } from './search'

export default function PropertySection({ title, icon, defaultOpen = true, expandable = true, animated = true, children }) {
  const [open, setOpen] = useState(defaultOpen)
  const query = useInspectorSearch()
  const visible = matchesQuery(children, query)
  if (query && visible.length === 0) return null

  if (!expandable) {
    return (
      <section className="overflow-hidden rounded-lg border border-border/60 bg-card/30">
        <div className="flex w-full items-center gap-2 px-3 py-2 text-[11px] font-semibold tracking-wider text-foreground/85 uppercase">
          <span className="text-muted-foreground">{icon}</span>
          <span className="truncate">{title}</span>
        </div>
        <div className="flex flex-col gap-2 px-2 pt-1.5 pb-2">{visible}</div>
      </section>
    )
  }

  return (
    <Collapsible open={open} onOpenChange={setOpen} asChild>
      <section className="overflow-hidden rounded-lg border border-border/60 bg-card/30">
        <CollapsibleTrigger asChild>
          <motion.button
            type="button"
            whileTap={{ scale: 0.985 }}
            aria-expanded={open}
            className="flex w-full items-center gap-2 px-3 py-2 text-[11px] font-semibold tracking-wider text-foreground/85 uppercase transition-colors hover:bg-accent/50"
          >
            <span className="text-muted-foreground">{icon}</span>
            <span className="truncate">{title}</span>
            <motion.span
              className="ml-auto text-muted-foreground"
              animate={{ rotate: open ? 0 : -90 }}
              transition={{ duration: 0.2 }}
            >
              <ChevronDown size={14} />
            </motion.span>
          </motion.button>
        </CollapsibleTrigger>
        <AnimatePresence initial={false}>
          {open ? (
            animated ? (
              <motion.div
                key="content"
                initial={{ height: 0, opacity: 0 }}
                animate={{ height: 'auto', opacity: 1 }}
                exit={{ height: 0, opacity: 0 }}
                transition={{ type: 'spring', stiffness: 380, damping: 34 }}
                className="overflow-hidden"
              >
                <div className="flex flex-col gap-2 px-2 pt-1.5 pb-2">{visible}</div>
              </motion.div>
            ) : (
              <div key="content" className="overflow-hidden">
                <div className="flex flex-col gap-2 px-2 pt-1.5 pb-2">{visible}</div>
              </div>
            )
          ) : null}
        </AnimatePresence>
      </section>
    </Collapsible>
  )
}
