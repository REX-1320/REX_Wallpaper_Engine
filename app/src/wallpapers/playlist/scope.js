// Pure playlist-resolution rules.
//
// The desktop is governed by a playlist only while the wallpaper ON THE
// DESKTOP belongs to it. The store's applied record is the truth; the path
// from `last-wallpaper.json` is a BOOT fallback for the window between main's
// re-apply and the gallery restoring its tiles.
//
// Keeping this pure makes the scoping rule testable without a browser:
// see scope.test.js. The engine imports it so the dock badge, the renderer
// rotation and the main-process plan can never disagree.

// 'None' is the value every wallpaper starts with, so it cannot by itself
// mean "do not rotate": the membership fallback is what keeps the rotation
// alive after a restart. Only a 'None' the user picked in this session is an
// explicit opt-out - the store marks that choice with playlist.explicit.
export const NO_PLAYLIST = { id: null, name: 'None', members: [] }

// Membership is keyed by the file's absolute path (case-insensitive on
// Windows) - the store's own ids are reassigned on every restart.
export function memberKey(value) {
  if (!value) return null
  return String(value).replace(/\//g, '\\').toLowerCase()
}

// The path of a wallpaper - the identity the playlist membership, the
// gallery library and the runtime all speak.
export function wallpaperPath(w) {
  return w?.filePath ?? w?.info?.filePath ?? w?.info?.source ?? null
}

// The wallpaper that is ON THE DESKTOP right now.
//
// The store knows it as appliedId - but at boot the store has not loaded the
// file yet (the gallery restores its tiles lazily, and main re-applied the
// last wallpaper before the renderer existed), so the path the runtime
// reports as "last applied" is the fallback.
export function appliedWallpaper(snapshot) {
  return snapshot?.wallpapers?.find((w) => w.id === snapshot.appliedId) ?? null
}

// The membership key of the wallpaper that is on the desktop - the cube the
// playlist strip outlines as "playing". The strip used to carry its own copy
// of these path rules; sharing them means the cube and the engine can never
// disagree about which file is running.
export function appliedMemberKey(snapshot) {
  return memberKey(wallpaperPath(appliedWallpaper(snapshot)))
}

// The playlist tag an apply writes onto the record. The SOURCE of the apply
// decides who governs the desktop - membership must never do it on its own:
//
//   gallery  -> an explicit 'None', so applying a playlist member from the
//               gallery stops the rotation instead of resurrecting its list
//   playlist -> that playlist's name
//   engine   -> null: a rotation keeps the tag the record already carries
//
// A playlist apply without a name cannot tag anything (returns null); the
// caller is expected to pass one.
export function playlistScopeForApply(source, playlistName = null) {
  if (source === 'gallery') return { currentPlaylist: 'None', explicit: true }
  if (source === 'playlist' && playlistName) {
    return { currentPlaylist: playlistName, explicit: true }
  }
  return null
}

// The playlist that governs a specific wallpaper record.
export function playlistFor(snapshot, wallpaper) {
  if (!wallpaper) return null
  const playlists = snapshot.playlists ?? []
  const name = wallpaper.playlist?.currentPlaylist
  // 'None' is BOTH the default value and a real sentinel playlist (pl-0) in
  // the store. An explicit None pick means "no rotation" - returning the pl-0
  // record would hand the engine that playlist's default Timer settings and
  // start rotating again.
  if (name === 'None' && wallpaper.playlist?.explicit) return NO_PLAYLIST
  if (name && name !== 'None') {
    const byName = playlists.find((p) => p.name === name)
    if (byName) return byName
  }
  const key = memberKey(wallpaperPath(wallpaper))
  if (!key) return null
  return playlists.find((p) => (p.members ?? []).some((m) => memberKey(m) === key)) ?? null
}

// The playlist that governs the desktop.
//
// A real applied record that belongs to no playlist means the desktop is NOT
// governed by one. The stale boot path must not resurrect the old rotation -
// that bug made a gallery apply keep following the previous playlist's
// Timer / scene-end rules. `lastAppliedPath` is consulted only while the
// store has no applied record at all (boot).
export function resolveDesktopPlaylist(snapshot, lastAppliedPath) {
  const applied = appliedWallpaper(snapshot)
  const byWallpaper = playlistFor(snapshot, applied)
  if (byWallpaper) return byWallpaper
  if (applied) return null
  const key = memberKey(lastAppliedPath)
  if (!key) return null
  return (snapshot.playlists ?? []).find((p) => (p.members ?? []).some((m) => memberKey(m) === key)) ?? null
}
