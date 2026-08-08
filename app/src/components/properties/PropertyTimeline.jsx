import { Slider } from '@/components/ui/slider'
import PropertyRow from './PropertyRow'

function formatTimecode(seconds) {
  const m = Math.floor(seconds / 60)
  const s = Math.floor(seconds % 60)
  const tenths = Math.floor((seconds % 1) * 10)
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}.${tenths}`
}

export default function PropertyTimeline({
  label,
  description,
  tooltip,
  value,
  onChange,
  min = 0,
  max = 60,
  step = 0.1,
}) {
  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <div className="flex w-36 items-center gap-2">
        <Slider
          min={min}
          max={max}
          step={step}
          value={[value]}
          onValueChange={([next]) => onChange(next)}
          className="flex-1"
        />
        <span className="w-11 shrink-0 text-right text-[10px] tabular-nums text-muted-foreground">
          {formatTimecode(value)}
        </span>
      </div>
    </PropertyRow>
  )
}
