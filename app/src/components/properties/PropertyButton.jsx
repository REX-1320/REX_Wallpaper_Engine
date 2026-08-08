import { Button } from '@/components/ui/button'

export default function PropertyButton({ children, onClick, variant = 'secondary', className }) {
  return (
    <Button
      type="button"
      variant={variant === 'danger' ? 'destructive' : variant}
      size="sm"
      className={`w-full${className ? ` ${className}` : ''}`}
      onClick={onClick}
    >
      {children}
    </Button>
  )
}
