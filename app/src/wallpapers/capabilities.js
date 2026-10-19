// Capability resolution for live wallpaper updates (audit 5.10 #15): the
// static per-type map in desktop.js is only a fallback — the runtime's real
// adapter set is authoritative when reachable. Intersect so type
// applicability is still enforced, and fall back to the static map when the
// runtime is unavailable (browser-only dev, runtime down). Kept pure so it
// can be node-tested without the Electron bridge.
export async function effectiveCapabilities(typeCaps, bridge) {
  const fallback = Array.isArray(typeCaps) ? typeCaps : []
  try {
    const live = await bridge?.getCapabilities?.()
    if (Array.isArray(live?.properties) && live.properties.length > 0) {
      return fallback.filter((p) => live.properties.includes(p))
    }
  } catch {
    // runtime unreachable — keep the static map
  }
  return fallback
}
