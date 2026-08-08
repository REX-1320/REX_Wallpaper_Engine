import { Tooltip, TooltipTrigger, TooltipContent } from '@/components/ui/tooltip'

export default function PropertyRow({ label, description, tooltip, children }) {
  const content = (
    <div className="flex min-w-0 flex-1 flex-col gap-0.5">
      <span className="text-xs font-medium text-foreground/90">{label}</span>
      {description ? (
        <span className="text-[10.5px] leading-tight text-muted-foreground">{description}</span>
      ) : null}
    </div>
  )

  return (
    <div className="flex items-center gap-3 rounded-md px-1.5 py-1 transition-colors hover:bg-accent/40">
      {tooltip ? (
        <Tooltip>
          <TooltipTrigger asChild>{content}</TooltipTrigger>
          <TooltipContent side="left" sideOffset={10}>
            {tooltip}
          </TooltipContent>
        </Tooltip>
      ) : (
        content
      )}
      <div className="flex min-w-24 shrink-0 items-center justify-end gap-1.5">{children}</div>
    </div>
  )
}
