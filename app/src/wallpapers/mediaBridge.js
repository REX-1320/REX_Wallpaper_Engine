const media = new Map()

export function registerMedia(id, element) {
  media.set(id, element)
}

export function unregisterMedia(id) {
  media.delete(id)
}

export function getMedia(id) {
  return media.get(id) ?? null
}

export function hasMedia(id) {
  return media.has(id)
}
