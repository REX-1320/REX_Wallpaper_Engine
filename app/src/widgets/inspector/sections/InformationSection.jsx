import { Info } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyText from '@/components/properties/PropertyText'

const INFO_FIELDS = [
  { key: 'name', label: 'Widget Name' },
  { key: 'author', label: 'Author' },
  { key: 'version', label: 'Version' },
  { key: 'license', label: 'License' },
  { key: 'package', label: 'Package' },
  { key: 'category', label: 'Category' },
  { key: 'tags', label: 'Tags' },
  { key: 'description', label: 'Description' },
  { key: 'lastUpdated', label: 'Last Updated' },
  { key: 'fileLocation', label: 'File Location', mono: true },
  { key: 'widgetId', label: 'Widget ID', mono: true },
]

export default function InformationSection({ widget }) {
  return (
    <PropertySection title="Information" icon={<Info size={15} />} defaultOpen={false}>
      {INFO_FIELDS.map((field) => (
        <PropertyText key={field.key} label={field.label} value={widget.info[field.key]} readOnly mono={field.mono} />
      ))}
    </PropertySection>
  )
}
