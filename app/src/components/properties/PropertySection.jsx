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
      <section className="prop-section">
        <div className="prop-section-head">
          <span className="prop-section-icon">{icon}</span>
          <span className="truncate">{title}</span>
        </div>
        <div className="prop-section-body">{visible}</div>
      </section>
    )
  }

  return (
    <Collapsible open={open} onOpenChange={setOpen} asChild>
      <section className="prop-section">
        <CollapsibleTrigger asChild>
          <motion.button
            type="button"
            whileTap={{ scale: 0.985 }}
            aria-expanded={open}
            className="prop-section-head"
          >
            <span className="prop-section-icon">{icon}</span>
            <span className="truncate">{title}</span>
            <motion.span
              className="prop-section-chevron"
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
                <div className="prop-section-body">{visible}</div>
              </motion.div>
            ) : (
              <div key="content" className="overflow-hidden">
                <div className="prop-section-body">{visible}</div>
              </div>
            )
          ) : null}
        </AnimatePresence>
      </section>
    </Collapsible>
  )
}
