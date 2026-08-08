import { Slider } from '@/components/ui/slider'
import PropertyRow from './PropertyRow'

export default function PropertySlider({
  label,
  description,
  tooltip,
  value,
  onChange,
  min = 0,
  max = 100,
  step = 1,
  suffix = '%',
}) {
  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <div className="flex w-28 items-center gap-2">
        <Slider
          min={min}
          max={max}
          step={step}
          value={[value]}
          onValueChange={([next]) => onChange(next)}
          className="flex-1"
        />
        <span className="w-9 shrink-0 text-right text-[11px] tabular-nums text-muted-foreground">
          {value}
          {suffix}
        </span>
      </div>
    </PropertyRow>
  )
}
