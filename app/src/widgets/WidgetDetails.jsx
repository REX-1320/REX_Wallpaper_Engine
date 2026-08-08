import { MousePointerClick } from 'lucide-react'
import { AnimatePresence, motion } from 'motion/react'
import { Badge } from '@/components/ui/badge'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Separator } from '@/components/ui/separator'
import { DEFAULT_ICON, getTemplate } from './registry'
import { useWidgetStore } from './store'

const LIVE_PROPS = [
  { label: 'Position', format: (w) => `${w.position.x}, ${w.position.y}` },
  { label: 'Size', format: (w) => `${w.position.width} × ${w.position.height}` },
  { label: 'Opacity', format: (w) => `${w.appearance.opacity}%` },
  { label: 'Transparency', format: (w) => w.appearance.transparencyMode },
  { label: 'Theme', format: (w) => w.appearance.theme },
  { label: 'Layer', format: (w) => `${w.layer.layer} · Z ${w.layer.zIndex}` },
]

const INFO_FIELDS = [
  { key: 'author', label: 'Author' },
  { key: 'version', label: 'Version' },
  { key: 'license', label: 'License' },
  { key: 'package', label: 'Package' },
  { key: 'tags', label: 'Tags' },
  { key: 'lastUpdated', label: 'Updated' },
  { key: 'widgetId', label: 'Widget ID' },
  { key: 'fileLocation', label: 'File Location' },
]

export default function WidgetDetails() {
  const { widgets, selectedId } = useWidgetStore()
  const widget = widgets.find((w) => w.id === selectedId) ?? null
  const template = widget ? getTemplate(widget.templateId) : null
  const Icon = template?.icon ?? DEFAULT_ICON

  return (
    <div className="widgets-details">
      <AnimatePresence mode="wait">
        {widget ? (
          <motion.div
            key={widget.id}
            initial={{ opacity: 0, x: -12 }}
            animate={{ opacity: 1, x: 0 }}
            exit={{ opacity: 0, x: 12 }}
            transition={{ duration: 0.18 }}
          >
            <Card className="gap-4 rounded-xl border-border/60 bg-card/40 py-4 backdrop-blur-xl">
              <CardHeader className="px-4">
                <div className="flex items-center gap-3">
                  <div className="flex size-11 shrink-0 items-center justify-center rounded-lg border border-border/60 bg-muted text-foreground">
                    <Icon className="size-5" />
                  </div>
                  <div className="min-w-0">
                    <CardTitle className="truncate text-sm">{widget.name}</CardTitle>
                    <div className="mt-1 flex items-center gap-1.5">
                      <Badge variant="secondary" className="text-[9px] tracking-wider uppercase">
                        {widget.category}
                      </Badge>
                    </div>
                  </div>
                </div>
                {widget.info.description ? (
                  <p className="mt-1 text-xs leading-relaxed text-muted-foreground">
                    {widget.info.description}
                  </p>
                ) : null}
              </CardHeader>

              <CardContent className="flex flex-col gap-3 px-4">
                <div className="grid grid-cols-2 gap-x-3 gap-y-2">
                  {LIVE_PROPS.map(({ label, format }) => (
                    <div key={label} className="min-w-0">
                      <div className="text-[9px] tracking-wider text-muted-foreground uppercase">{label}</div>
                      <div className="truncate text-xs tabular-nums text-foreground/90">
                        {format(widget)}
                      </div>
                    </div>
                  ))}
                </div>

                <Separator />

                <div className="flex flex-col gap-2">
                  {INFO_FIELDS.map(({ key, label }) => (
                    <div key={key} className="flex items-baseline justify-between gap-3">
                      <span className="shrink-0 text-[10px] tracking-wider text-muted-foreground uppercase">
                        {label}
                      </span>
                      <span
                        className="min-w-0 truncate text-right text-[11px] font-mono text-foreground/85"
                        title={String(widget.info[key])}
                      >
                        {String(widget.info[key])}
                      </span>
                    </div>
                  ))}
                </div>
              </CardContent>
            </Card>
          </motion.div>
        ) : (
          <motion.div
            key="empty"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="flex h-full min-h-52 flex-col items-center justify-center gap-3 rounded-xl border border-dashed border-border/60 bg-card/30 p-6 text-center"
          >
            <div className="flex size-10 items-center justify-center rounded-lg bg-muted text-muted-foreground">
              <MousePointerClick className="size-5" />
            </div>
            <p className="text-xs leading-relaxed text-muted-foreground">
              Select a widget to view its details.
            </p>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  )
}
