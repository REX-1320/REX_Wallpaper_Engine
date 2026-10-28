import test from 'node:test'
import assert from 'node:assert/strict'

import {
  NO_PLAYLIST,
  appliedMemberKey,
  memberKey,
  playlistFor,
  playlistScopeForApply,
  resolveDesktopPlaylist,
} from './scope.js'

function makeSnapshot(overrides = {}) {
  return {
    wallpapers: [],
    playlists: [],
    selectedId: null,
    appliedId: null,
    ...overrides,
  }
}

const PLAYLIST = {
  id: 'pl-2',
  name: 'Rain',
  members: ['C:\\Wallpapers\\rain.mp4', 'C:\\Wallpapers\\storm.mp4'],
}

test('memberKey normalises Windows separators and case', () => {
  assert.equal(memberKey('C:/Wallpapers/Rain.MP4'), 'c:\\wallpapers\\rain.mp4')
  assert.equal(memberKey(null), null)
})

// The playlist strip outlines the cube whose membership key equals the key of
// the applied record. The class expression is driven by this one value, so a
// drift in the path rules silently removes the gold ring from the playing cube.
test('appliedMemberKey keys the playing cube like the playlist membership', () => {
  const snapshot = makeSnapshot({
    appliedId: 'file-1',
    wallpapers: [{ id: 'file-1', filePath: 'C:/Wallpapers/Rain.MP4' }],
  })

  assert.equal(appliedMemberKey(snapshot), memberKey(PLAYLIST.members[0]))
})

test('appliedMemberKey is null until a wallpaper is applied', () => {
  assert.equal(appliedMemberKey(makeSnapshot()), null)

  // A single-click preview is not "playing": only an apply may outline a cube.
  const preview = makeSnapshot({
    selectedId: 'file-1',
    wallpapers: [{ id: 'file-1', filePath: 'C:\\Wallpapers\\rain.mp4' }],
  })
  assert.equal(appliedMemberKey(preview), null)
})

test('appliedMemberKey falls back to info.filePath for a record without a root path', () => {
  const snapshot = makeSnapshot({
    appliedId: 'file-1',
    wallpapers: [{ id: 'file-1', info: { filePath: 'C:\\Wallpapers\\storm.mp4' } }],
  })

  assert.equal(appliedMemberKey(snapshot), memberKey(PLAYLIST.members[1]))
})

test('an applied playlist member resolves to its playlist', () => {
  const snapshot = makeSnapshot({
    appliedId: 'file-1',
    wallpapers: [{ id: 'file-1', filePath: 'C:\\Wallpapers\\rain.mp4' }],
    playlists: [PLAYLIST],
  })

  assert.equal(resolveDesktopPlaylist(snapshot, null)?.id, 'pl-2')
})

test('an applied gallery file outside every playlist stops the rotation', () => {
  // Regression: the desktop file is a real gallery record, but it is not a
  // member. The stale boot path still points at the old playlist, and the old
  // engine fell back to it - so the playlist kept rotating a gallery file.
  const snapshot = makeSnapshot({
    appliedId: 'file-9',
    wallpapers: [{ id: 'file-9', filePath: 'C:\\Wallpapers\\gallery-only.png' }],
    playlists: [PLAYLIST],
  })

  assert.equal(resolveDesktopPlaylist(snapshot, 'C:\\Wallpapers\\rain.mp4'), null)
})

test('the boot path fallback still resolves a playlist when no record is applied', () => {
  const snapshot = makeSnapshot({ playlists: [PLAYLIST] })

  assert.equal(resolveDesktopPlaylist(snapshot, 'C:\\Wallpapers\\storm.mp4')?.id, 'pl-2')
})

test('the boot path fallback is ignored once a gallery record is applied', () => {
  const snapshot = makeSnapshot({
    appliedId: 'file-9',
    wallpapers: [{ id: 'file-9', filePath: 'C:\\Wallpapers\\gallery-only.png' }],
    playlists: [PLAYLIST],
  })

  // Even with a path that belongs to the playlist, the applied record wins.
  assert.equal(resolveDesktopPlaylist(snapshot, 'C:\\Wallpapers\\rain.mp4'), null)
})

test('a preview selection never governs the desktop playlist', () => {
  const snapshot = makeSnapshot({
    appliedId: 'file-9',
    selectedId: 'file-1',
    wallpapers: [
      { id: 'file-9', filePath: 'C:\\Wallpapers\\gallery-only.png' },
      { id: 'file-1', filePath: 'C:\\Wallpapers\\rain.mp4' },
    ],
    playlists: [PLAYLIST],
  })

  assert.equal(resolveDesktopPlaylist(snapshot, null), null)
})

test('an explicit None pick is an opt-out even for a playlist member', () => {
  const wallpaper = {
    id: 'file-1',
    filePath: 'C:\\Wallpapers\\rain.mp4',
    playlist: { currentPlaylist: 'None', explicit: true },
  }
  // The store keeps a REAL 'None' sentinel playlist (pl-0) with its own
  // default settings. Returning it would restart a Timer on a stopped desktop.
  const noneSentinel = {
    id: 'pl-0',
    name: 'None',
    members: ['C:\\Wallpapers\\other.mp4', 'C:\\Wallpapers\\more.mp4'],
  }
  const snapshot = makeSnapshot({
    appliedId: 'file-1',
    wallpapers: [wallpaper],
    playlists: [noneSentinel, PLAYLIST],
  })

  const resolved = playlistFor(snapshot, wallpaper)
  assert.equal(resolved, NO_PLAYLIST)
  assert.equal(resolved.id, null)
  assert.equal(resolveDesktopPlaylist(snapshot, wallpaper.filePath), NO_PLAYLIST)
  assert.equal(resolveDesktopPlaylist(snapshot, wallpaper.filePath)?.id, null)
})

test('an explicit playlist name wins over membership', () => {
  const other = { id: 'pl-3', name: 'Space', members: [] }
  const wallpaper = {
    id: 'file-1',
    filePath: 'C:\\Wallpapers\\rain.mp4',
    playlist: { currentPlaylist: 'Space' },
  }
  const snapshot = makeSnapshot({
    appliedId: 'file-1',
    wallpapers: [wallpaper],
    playlists: [PLAYLIST, other],
  })

  assert.equal(playlistFor(snapshot, wallpaper)?.id, 'pl-3')
})

// Regression: applying a playlist MEMBER from the gallery used to leave the
// record's default 'None', and playlistFor's membership fallback then handed
// the engine the playlist - so the desktop kept rotating under a playlist the
// user never applied. The apply's SOURCE must decide the governance.
test('a gallery apply opts a playlist member out of its playlist', () => {
  const wallpaper = {
    id: 'file-1',
    filePath: 'C:\\Wallpapers\\rain.mp4',
    playlist: { currentPlaylist: 'Rain', explicit: true },
  }
  Object.assign(wallpaper.playlist, playlistScopeForApply('gallery'))

  assert.deepEqual(wallpaper.playlist, { currentPlaylist: 'None', explicit: true })

  const snapshot = makeSnapshot({
    appliedId: 'file-1',
    wallpapers: [wallpaper],
    playlists: [PLAYLIST],
  })
  assert.equal(playlistFor(snapshot, wallpaper), NO_PLAYLIST)
  assert.equal(resolveDesktopPlaylist(snapshot, wallpaper.filePath), NO_PLAYLIST)
  assert.equal(resolveDesktopPlaylist(snapshot, wallpaper.filePath)?.id, null)
})

test('a playlist apply tags the member with that playlist', () => {
  const wallpaper = {
    id: 'file-1',
    filePath: 'C:\\Wallpapers\\rain.mp4',
    playlist: { currentPlaylist: 'None' },
  }
  Object.assign(wallpaper.playlist, playlistScopeForApply('playlist', 'Rain'))

  assert.deepEqual(wallpaper.playlist, { currentPlaylist: 'Rain', explicit: true })

  const snapshot = makeSnapshot({
    appliedId: 'file-1',
    wallpapers: [wallpaper],
    playlists: [PLAYLIST],
  })
  assert.equal(playlistFor(snapshot, wallpaper)?.id, 'pl-2')
})

test('an engine rotation keeps the record tag it already has', () => {
  assert.equal(playlistScopeForApply('engine'), null)
  // A playlist apply with no name cannot tag anything; the caller must not
  // silently turn the desktop into a gallery apply either.
  assert.equal(playlistScopeForApply('playlist', null), null)
})
