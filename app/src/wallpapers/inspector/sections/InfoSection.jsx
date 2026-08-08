import { FolderOpen, FileCode2, Info } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyText from '@/components/properties/PropertyText'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyButton from '@/components/properties/PropertyButton'
import { TYPE_META } from '../../registry'
import { updateWallpaperProperty } from '../../store'

const INFO_FIELDS = [
  { key: 'name', label: 'Wallpaper Name' },
  { key: 'author', label: 'Author' },
  { key: 'source', label: 'Source' },
  { key: 'version', label: 'Version' },
  { key: 'license', label: 'License' },
  { key: 'category', label: 'Category' },
  { key: 'tags', label: 'Tags' },
  { key: 'description', label: 'Description' },
  { key: 'resolution', label: 'Resolution' },
  { key: 'aspectRatio', label: 'Aspect Ratio' },
  { key: 'duration', label: 'Duration' },
  { key: 'fps', label: 'FPS' },
  { key: 'codec', label: 'Codec' },
  { key: 'fileSize', label: 'File Size' },
  { key: 'dateAdded', label: 'Date Added' },
  { key: 'lastModified', label: 'Last Modified' },
  { key: 'rendererType', label: 'Renderer Type' },
  { key: 'supportedMonitors', label: 'Supported Monitors' },
]

export default function InfoSection({ wallpaper }) {
  const { info } = wallpaper
  const meta = TYPE_META[wallpaper.type]
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['info', key], value)

  return (
    <PropertySection title="Wallpaper Information" icon={<Info size={15} />} defaultOpen={false}>
      <PropertyGroup title="Thumbnail">
        <div className="flex items-center gap-3 px-1 py-1">
          <div
            className="flex size-14 shrink-0 items-center justify-center rounded-lg border border-border/60 text-white shadow-sm"
            style={{ background: meta.gradient }}
          >
            <meta.icon className="size-6" />
          </div>
          <div className="flex min-w-0 flex-col gap-0.5">
            <span className="truncate text-xs font-semibold text-foreground">{info.name}</span>
            <span className="text-[10px] tracking-wider text-muted-foreground uppercase">
              {meta.label} · {info.rendererType}
            </span>
          </div>
        </div>
      </PropertyGroup>
      <PropertyToggle label="Favorite" checked={info.favorite} onChange={set('favorite')} />
      <PropertySlider
        label="Rating"
        value={info.rating}
        onChange={set('rating')}
        min={0}
        max={5}
        step={0.5}
        suffix="★"
      />
      {INFO_FIELDS.map(({ key, label }) => (
        <PropertyText key={key} label={label} value={info[key]} readOnly />
      ))}
      <div className="grid grid-cols-2 gap-2 pt-1">
        <PropertyButton variant="outline" onClick={() => {}}>
          <FolderOpen size={14} /> Open Folder
        </PropertyButton>
        <PropertyButton variant="outline" onClick={() => {}}>
          <FileCode2 size={14} /> Open Metadata
        </PropertyButton>
      </div>
    </PropertySection>
  )
}
