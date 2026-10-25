import { useState } from 'react'
import { Copy, Folder, FolderOpen } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyButton from '@/components/properties/PropertyButton'
import { notifyError } from '../../notify'

export default function FileSection({ wallpaper }) {
  const [copied, setCopied] = useState(false)
  const filePath = typeof wallpaper?.filePath === 'string' ? wallpaper.filePath : ''

  const copyPath = async () => {
    if (!filePath) return
    try {
      await navigator.clipboard.writeText(filePath)
      setCopied(true)
      setTimeout(() => setCopied(false), 1200)
    } catch (err) {
      notifyError('Could not copy the path', err?.message ?? String(err))
    }
  }

  return (
    <PropertySection title="File" icon={<Folder size={15} />} defaultOpen={false}>
      <div key="path" className="insp-path" aria-label="File path" title={filePath || 'No file path'}>
        {filePath || 'No file path'}
      </div>
      <div key="actions" className="grid grid-cols-2 gap-2">
        <PropertyButton onClick={copyPath} disabled={!filePath}>
          <Copy size={14} /> {copied ? 'Copied' : 'Copy path'}
        </PropertyButton>
        <PropertyButton
          onClick={() => filePath && window.paiintDesktop?.showInFolder?.(filePath)}
          disabled={!filePath}
        >
          <FolderOpen size={14} /> Open location
        </PropertyButton>
      </div>
    </PropertySection>
  )
}
