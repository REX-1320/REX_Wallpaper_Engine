import { Zap, RotateCw, Copy, Download, RotateCcw, FolderOpen, FileCode2, Trash2 } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyButton from '@/components/properties/PropertyButton'
import { duplicateWidget, resetWidget, deleteWidget } from '../../store'

const noop = () => {}

export default function ActionsSection({ widget }) {
  return (
    <PropertySection title="Actions" icon={<Zap size={15} />} defaultOpen={false}>
      <div className="grid grid-cols-2 gap-2">
        <PropertyButton onClick={noop}>
          <RotateCw size={14} /> Reload Widget
        </PropertyButton>
        <PropertyButton onClick={() => duplicateWidget(widget.id)}>
          <Copy size={14} /> Duplicate
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <Download size={14} /> Export
        </PropertyButton>
        <PropertyButton onClick={() => resetWidget(widget.id)}>
          <RotateCcw size={14} /> Reset Settings
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <FolderOpen size={14} /> Open Folder
        </PropertyButton>
        <PropertyButton onClick={noop}>
          <FileCode2 size={14} /> Edit Source
        </PropertyButton>
        <PropertyButton variant="danger" onClick={() => deleteWidget(widget.id)} className="col-span-2">
          <Trash2 size={14} /> Delete Widget
        </PropertyButton>
      </div>
    </PropertySection>
  )
}
