import test from 'node:test'
import assert from 'node:assert/strict'

import { NO_PLAYLIST, memberKey, playlistFor, resolveDesktopPlaylist } from './scope.js'

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
