import {
  Copy,
  Download,
  FolderOpen,
  Package,
  Save,
  Share2,
  Trash2,
  Upload,
  FileCode2,
  HardDrive,
} from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyButton from '@/components/properties/PropertyButton'
import { deleteWallpaper, duplicateWallpaper } from '../../store'

const noop = () => {}

export default function ActionsSection({ wallpaper }) {
  return (
    <PropertySection title="Actions" icon={<Save size={15} />} defaultOpen={false}>
      <div className="grid grid-cols-2 gap-2">
        <PropertyButton onClick={() => duplicateWallpaper(wallpaper.id)}>
          <Copy size={14} /> Duplicate
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <Save size={14} /> Save Preset
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <Upload size={14} /> Load Preset
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <Download size={14} /> Export Preset
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <Package size={14} /> Export Package
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <Share2 size={14} /> Share
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <HardDrive size={14} /> Backup
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <FolderOpen size={14} /> Open Folder
        </PropertyButton>
        <PropertyButton onClick={noop} className="col-span-2">
          <FileCode2 size={14} /> Reveal Metadata
        </PropertyButton>
        <PropertyButton
          variant="danger"
          className="col-span-2"
          onClick={() => deleteWallpaper(wallpaper.id)}
        >
          <Trash2 size={14} /> Delete Wallpaper
        </PropertyButton>
      </div>
    </PropertySection>
  )
}
