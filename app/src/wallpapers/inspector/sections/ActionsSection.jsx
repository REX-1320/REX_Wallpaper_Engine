import { Copy, FolderOpen, Save, Trash2 } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyButton from '@/components/properties/PropertyButton'
import { deleteWallpaper, duplicateWallpaper } from '../../store'

export default function ActionsSection({ wallpaper }) {
  // Deferred features (presets, share, backup, metadata) used to render as
  // dead buttons here (audit 5.10 #16). Only real actions remain; Open Folder
  // was wireable with the existing shell:show-in-folder IPC.
  const canReveal = typeof wallpaper?.filePath === 'string' && wallpaper.filePath.length > 0
  return (
    <PropertySection title="Actions" icon={<Save size={15} />} defaultOpen={false}>
      <div className="grid grid-cols-2 gap-2">
        <PropertyButton onClick={() => duplicateWallpaper(wallpaper.id)}>
          <Copy size={14} /> Duplicate
        </PropertyButton>
        <PropertyButton
          disabled={!canReveal}
          onClick={() => canReveal && window.rexDesktop?.showInFolder?.(wallpaper.filePath)}
        >
          <FolderOpen size={14} /> Open Folder
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
