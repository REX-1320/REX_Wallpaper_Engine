import { ChevronDown, ChevronUp } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import PropertyRow from './PropertyRow'

export default function PropertyNumber({
  label,
  description,
  tooltip,
  value,
  onChange,
  min = -Infinity,
  max = Infinity,
  step = 1,
  suffix,
}) {
  const set = (next) => {
    const clamped = Math.min(max, Math.max(min, Number.isNaN(next) ? value : next))
    onChange(clamped)
  }

  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <div className="flex items-center gap-0.5">
        <Button
          variant="ghost"
          size="icon-xs"
          className="size-6"
          aria-label={`Decrease ${label}`}
          disabled={value <= min}
          onClick={() => set(value - step)}
        >
          <ChevronDown />
        </Button>
        <Input
          type="number"
          min={min}
          max={max}
          step={step}
          className="h-6 w-16 px-1.5 text-right text-xs tabular-nums"
          value={value}
          onChange={(e) => set(Number(e.target.value))}
        />
        <Button
          variant="ghost"
          size="icon-xs"
          className="size-6"
          aria-label={`Increase ${label}`}
          disabled={value >= max}
          onClick={() => set(value + step)}
        >
          <ChevronUp />
        </Button>
        {suffix ? <span className="ml-1 text-[10px] text-muted-foreground">{suffix}</span> : null}
      </div>
    </PropertyRow>
  )
}
