// Palette guard.
//
// Two jobs:
//   1. No raw colour literals in the app's own CSS (the palette in
//      index.css is the single source of truth). Shadows, gradients and
//      `transparent` are allowed — they are depth cues, not surfaces.
//   2. Every text step in the palette clears WCAG 2.2 AA (4.5:1) against
//      the surface it is used on, and the disabled step is exempt (inactive
//      UI) but must never be the only signal.
//
// usage: node app/tools/check-palette.cjs
const fs = require('node:fs')
const path = require('node:path')

const SRC = path.join(__dirname, '..', 'src')

// ── colour maths ───────────────────────────────────────────────────────────
// The palette is stored as ramps of channel triplets, so a step can be used at
// any alpha: `rgb(var(--obsidian-25))` / `rgba(var(--obsidian-25), 0.07)`.
const indexCss = fs.readFileSync(path.join(SRC, 'index.css'), 'utf8')
const channels = new Map()
for (const m of indexCss.matchAll(/(--[\w-]+)\s*:\s*(\d+)\s+(\d+)\s+(\d+)\s*;/g)) {
  channels.set(m[1], { r: +m[2], g: +m[3], b: +m[4], a: 1 })
}

const parse = (value) => {
  const v = value.trim()
  let m = v.match(/^#([0-9a-f]{3,8})$/i)
  if (m) {
    let h = m[1]
    if (h.length === 3) h = [...h].map((c) => c + c).join('')
    return { r: parseInt(h.slice(0, 2), 16), g: parseInt(h.slice(2, 4), 16), b: parseInt(h.slice(4, 6), 16), a: h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1 }
  }
  m = v.match(/^rgba?\(\s*var\((--[\w-]+)\)\s*(?:,\s*([\d.]+))?\s*\)$/i)
  if (m) {
    const c = channels.get(m[1])
    if (!c) return null
    return { ...c, a: m[2] === undefined ? 1 : +m[2] }
  }
  // The modern slash form: rgb(var(--step) / alpha)
  m = v.match(/^rgba?\(\s*var\((--[\w-]+)\)\s*\/\s*([\d.]+)\s*\)$/i)
  if (m) {
    const c = channels.get(m[1])
    if (!c) return null
    return { ...c, a: +m[2] }
  }
  m = v.match(/^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+))?\s*\)$/i)
  if (m) return { r: +m[1], g: +m[2], b: +m[3], a: m[4] === undefined ? 1 : +m[4] }
  m = v.match(/^oklch\(\s*([\d.]+)(?:\s+([\d.]+))?\s+([\d.]+)\s*\)$/i)
  if (m) {
    const L = +m[1]
    const g = Math.max(0, Math.min(255, Math.round(L * 255)))
    return { r: g, g, b: g, a: 1 }
  }
  return null
}
const over = (fg, bg) => ({ r: fg.r * fg.a + bg.r * (1 - fg.a), g: fg.g * fg.a + bg.g * (1 - fg.a), b: fg.b * fg.a + bg.b * (1 - fg.a), a: 1 })
const lin = (c) => {
  c /= 255
  return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4
}
const lum = (c) => 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
const ratio = (a, b) => {
  const l1 = lum(a)
  const l2 = lum(b)
  const [hi, lo] = l1 > l2 ? [l1, l2] : [l2, l1]
  return (hi + 0.05) / (lo + 0.05)
}

// ── 1. literals in the app's own CSS ───────────────────────────────────────
const cssFiles = []
const walk = (d) => {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name)
    if (e.isDirectory()) walk(p)
    else if (e.name.endsWith('.css')) cssFiles.push(p)
  }
}
walk(SRC)

// Every token a stylesheet references must exist somewhere.
const defined = new Set()
for (const file of cssFiles) {
  for (const m of fs.readFileSync(file, 'utf8').matchAll(/(--[\w-]+)\s*:/g)) defined.add(m[1])
}
const undefinedTokens = new Map()
for (const file of cssFiles) {
  for (const m of fs.readFileSync(file, 'utf8').matchAll(/var\((--[\w-]+)\)/g)) {
    if (!defined.has(m[1])) undefinedTokens.set(m[1], (undefinedTokens.get(m[1]) || 0) + 1)
  }
}

// Shadows and filters are depth cues, not surfaces — they keep their own
// geometry and are allowed to carry a literal.
const SHADOW_PROPS = new Set(['box-shadow', 'text-shadow', 'filter', 'background-image'])
const violations = []
for (const file of cssFiles) {
  if (file.endsWith('index.css')) continue // the palette itself
  const css = fs.readFileSync(file, 'utf8')
  css.replace(/([\w-]+)\s*:\s*([^;{}]+)([;}])/g, (m, prop, value) => {
    const v = value.trim()
    if (v.includes('var(') || v.includes('gradient')) return m
    if (/transparent|none|inherit|currentColor/.test(v)) return m
    if (SHADOW_PROPS.has(prop.trim().toLowerCase())) return m
    if (/#[0-9a-fA-F]{3,8}\b|rgba?\(|oklch\(|hsla?\(/.test(v)) {
      violations.push(`${file.replace(/\\/g, '/')}: ${prop.trim()}: ${v.replace(/\s+/g, ' ')}`)
    }
    return m
  })
}

// ── 2. the palette's own contrast ─────────────────────────────────────────
const index = indexCss
const tokens = {}
for (const m of index.matchAll(/--([\w-]+):\s*([^;]+);/g)) {
  const raw = m[2].trim()
  if (raw.startsWith('var(') && !raw.includes('rgb(var(')) continue
  tokens[`--${m[1]}`] = raw
}
const resolved = (name) => {
  const raw = tokens[name]
  if (!raw) return null
  return parse(raw)
}
const composite = (name) => {
  const c = resolved(name)
  if (!c) return null
  return c.a < 1 ? over(c, { r: 0, g: 0, b: 0, a: 1 }) : c
}

const TEXT_ON = ['--surface-shell', '--surface-sidebar', '--surface-content', '--surface-raised']
const textChecks = [
  ['--text-primary', 4.5],
  ['--text-secondary', 4.5],
  ['--text-tertiary', 4.5],
  ['--text-disabled', 0], // inactive UI — exempt from 1.4.3
]
const contrastIssues = []
for (const surface of TEXT_ON) {
  const bg = composite(surface)
  if (!bg) {
    contrastIssues.push(`${surface} could not be resolved — the contrast check would be vacuous`)
    continue
  }
  for (const [text, min] of textChecks) {
    const fg = resolved(text)
    if (!fg) {
      contrastIssues.push(`${text} could not be resolved`)
      continue
    }
    const r = ratio(over(fg, bg), bg)
    if (r < min) contrastIssues.push(`${text} on ${surface} = ${r.toFixed(2)}:1 (needs ${min || 'n/a (exempt)'})`)
  }
}

// The worst case: the panels are translucent over the system Mica, and the
// Mica carries the user's wallpaper. A WHITE wallpaper is the harshest
// backdrop, so the text must clear 4.5:1 over the panel composited on white.
for (const surface of TEXT_ON) {
  const c = resolved(surface)
  if (!c) continue
  const overWhite = over(c, { r: 255, g: 255, b: 255, a: 1 })
  for (const [text, min] of textChecks) {
    if (!min) continue
    const fg = resolved(text)
    if (!fg) continue
    const r = ratio(over(fg, overWhite), overWhite)
    if (r < min) {
      contrastIssues.push(`${text} on ${surface} over a WHITE wallpaper = ${r.toFixed(2)}:1 (needs ${min})`)
    }
  }
}
for (const [accent, min] of [['--gold', 4.5], ['--gold-bright', 4.5], ['--danger', 4.5], ['--warning', 4.5], ['--success', 4.5]]) {
  const fg = resolved(accent)
  const bg = composite('--surface-shell')
  if (!fg || !bg) continue
  const r = ratio(over(fg, bg), bg)
  if (r < min) contrastIssues.push(`${accent} on --surface-shell = ${r.toFixed(2)}:1 (needs ${min})`)
}

// ── report ─────────────────────────────────────────────────────────────────
let failed = false
if (violations.length) {
  failed = true
  console.log(`\n✗ ${violations.length} raw colour literal(s) outside the palette:`)
  for (const v of violations) console.log(`   ${v}`)
} else {
  console.log('✓ no raw colour literals outside the palette')
}
if (undefinedTokens.size) {
  failed = true
  console.log(`\n✗ ${undefinedTokens.size} undefined token reference(s):`)
  for (const [t, n] of undefinedTokens) console.log(`   var(${t}) × ${n}`)
} else {
  console.log('✓ every var() reference resolves to a defined token')
}
if (contrastIssues.length) {
  failed = true
  console.log(`\n✗ ${contrastIssues.length} contrast failure(s):`)
  for (const c of contrastIssues) console.log(`   ${c}`)
} else {
  console.log('✓ every palette text step clears WCAG AA on every surface')
}
console.log(`\n${Object.keys(tokens).length} tokens · ${cssFiles.length} CSS files checked`)
process.exit(failed ? 1 : 0)
