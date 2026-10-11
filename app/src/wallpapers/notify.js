// User-facing error notifications (P1 error-to-toast path). Wallpaper
// failures used to end at console.warn (desktop.js) — invisible to users.
// sonner is already a dependency and <Toaster> is mounted at the App root;
// outside the desktop app (or if sonner throws) this falls back to console
// so diagnostic output is never lost.

import { toast } from 'sonner'

export function notifyError(title, detail) {
  const message = detail ? `${title}: ${detail}` : String(title)
  // Renderer console always keeps a record (devtools + bug reports)...
  console.warn(message)
  // ...and the desktop UI shows it when a Toaster is mounted.
  try {
    if (toast && typeof toast.error === 'function') {
      toast.error(title, detail ? { description: String(detail) } : undefined)
    }
  } catch {
    // console already has it
  }
}
