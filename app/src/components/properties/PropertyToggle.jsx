import { Switch } from '@/components/ui/switch'
import PropertyRow from './PropertyRow'

export default function PropertyToggle({ label, description, tooltip, checked, onChange }) {
  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <Switch size="sm" checked={checked} onCheckedChange={onChange} />
    </PropertyRow>
  )
}
