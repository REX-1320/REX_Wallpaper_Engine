import { useRef } from 'react'
import { FolderOpen } from 'lucide-react'
import { Button } from '@/components/ui/button'
import PropertyRow from './PropertyRow'

export default function PropertyFilePicker({
  label,
  description,
  tooltip,
  value,
  onChange,
  accept,
}) {
  const inputRef = useRef(null)

  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <div className="flex min-w-0 items-center gap-1.5">
        <span
          className="max-w-24 truncate text-[10px] font-mono text-muted-foreground"
          title={value}
        >
          {value}
        </span>
        <Button
          variant="ghost"
          size="icon-xs"
          className="size-6"
          aria-label={`Browse ${label}`}
          onClick={() => inputRef.current?.click()}
        >
          <FolderOpen />
        </Button>
        <input
          ref={inputRef}
          type="file"
          hidden
          accept={accept}
          onChange={(e) => {
            const file = e.target.files?.[0]
            if (file) onChange(file.name)
            e.target.value = ''
          }}
        />
      </div>
    </PropertyRow>
  )
}
