export default function PropertyGroup({ title, children }) {
  return (
    <div className="flex flex-col gap-1.5 rounded-lg border border-border/60 bg-card/40 p-2">
      {title ? (
        <div className="px-1 text-[10px] font-semibold tracking-wider text-muted-foreground uppercase">
          {title}
        </div>
      ) : null}
      {children}
    </div>
  )
}
