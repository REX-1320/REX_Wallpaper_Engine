// One-off migration: replace the app's ad-hoc colour literals with the REX
// palette tokens. The mapping is ROLE-AWARE: the same literal means a text
// colour in one property and a fill in another, so a blind find/replace would
// be wrong.
//
// usage: node app/tools/migrate-palette.cjs [--dry]
const fs = require('node:fs')
const path = require('node:path')

const DRY = process.argv.includes('--dry')

const W = (a) => `rgba(255, 255, 255, ${a})`
const B = (a) => `rgba(0, 0, 0, ${a})`

// ── Role tables ────────────────────────────────────────────────────────────
const TEXT = {
  '#fff': '--text-primary',
  '#ffffff': '--text-primary',
  [W(0.95)]: '--text-primary',
  [W(0.92)]: '--text-primary',
  [W(0.9)]: '--text-primary',
  [W(0.88)]: '--text-secondary',
  [W(0.85)]: '--text-secondary',
  [W(0.8)]: '--text-secondary',
  [W(0.75)]: '--text-secondary',
  [W(0.7)]: '--text-tertiary',
  [W(0.65)]: '--text-tertiary',
  [W(0.6)]: '--text-tertiary',
  [W(0.55)]: '--text-tertiary',
  [W(0.5)]: '--text-tertiary',
  [W(0.45)]: '--text-disabled',
  [W(0.4)]: '--text-disabled',
  [W(0.35)]: '--text-disabled',
  [W(0.3)]: '--text-disabled',
  '#000': '--text-inverse',
  '#000000': '--text-inverse',
  '#0b1020': '--text-inverse',
  '#1a1a1a': '--text-primary',
  '#bde9ff': '--accent-strong',
  [B(0.85)]: '--text-inverse-secondary',
  [B(0.82)]: '--text-inverse-secondary',
  [B(0.72)]: '--text-inverse-secondary',
  [B(0.6)]: '--text-inverse-tertiary',
  [B(0.45)]: '--text-inverse-disabled',
  [B(0.4)]: '--text-inverse-disabled',
  '#7dd3fc': '--accent',
  '#38bdf8': '--accent-fill',
  '#fca5a5': '--danger',
  '#ef4444': '--danger',
  'rgb(239, 68, 68)': '--danger',
  '#fbbf24': '--warning',
  '#34d399': '--success',
}

const FILL = {
  '#fff': '--fill-inverse-solid',
  '#ffffff': '--fill-inverse-solid',
  [W(0.18)]: '--fill-selected',  [W(0.16)]: '--fill-selected',
  [W(0.14)]: '--fill-strong',
  [W(0.12)]: '--fill-strong',
  [W(0.1)]: '--fill-hover',
  [W(0.08)]: '--fill-hover',
  [W(0.06)]: '--fill-rest',
  [W(0.05)]: '--fill-rest',
  [W(0.04)]: '--fill-pressed',
  [W(0.03)]: '--fill-pressed',
  [B(0.06)]: '--fill-inverse',
  [B(0.85)]: '--fill-inverse-strong',
  'rgba(56, 189, 248, 0.14)': '--accent-muted',
  'rgba(56, 189, 248, 0.12)': '--accent-muted',
  'rgba(56, 189, 248, 0.35)': '--accent-muted',
  'rgba(125, 211, 252, 0.14)': '--accent-muted',
  'rgba(239, 68, 68, 0.18)': '--danger-muted',
  'rgba(0, 0, 0, 0.25)': '--fill-inverse-strong',
  // Surfaces — the app's base/content layers and the scrims.
  '#000': '--surface-base',
  '#000000': '--surface-base',
  '#12141d': '--surface-content',
  'rgba(8, 8, 12, 0.78)': '--surface-base',
  'rgba(8, 8, 12, 0.9)': '--surface-base',
  'rgba(10, 12, 18, 0.8)': '--surface-base',
  'rgba(12, 14, 22, 0.82)': '--surface-base',
  'rgba(12, 12, 18, 0.95)': '--surface-raised',
  'rgba(12, 12, 18, 0.97)': '--surface-raised',
  'rgba(14, 16, 24, 0.92)': '--surface-raised',
  'rgba(10, 12, 18, 0.5)': '--surface-sunken',
  'rgba(10, 12, 18, 0.52)': '--surface-sunken',
  'rgba(10, 12, 18, 0.55)': '--surface-sunken',
  [B(0.5)]: '--surface-sunken',
  [B(0.6)]: '--surface-sunken',
  [B(0.55)]: '--surface-smoke',
  [B(0.66)]: '--fill-inverse-strong',
  [B(0.45)]: '--fill-inverse-strong',
  [B(0.35)]: '--fill-inverse',
  [B(0.12)]: '--fill-inverse',
  'rgba(255, 255, 255, 0.25)': '--fill-strong',
  'rgba(255, 255, 255, 0.34)': '--fill-selected',
  'rgba(255, 255, 255, 0.22)': '--fill-strong',
  'rgba(255, 255, 255, 0.13)': '--fill-hover',
  'rgba(255, 255, 255, 0.09)': '--fill-hover',
  'rgba(255, 255, 255, 0.07)': '--fill-rest',
  'rgba(10, 10, 14, 0.95)': '--surface-raised',
  'rgba(56, 189, 248, 0.3)': '--accent-muted',
  'rgba(56, 189, 248, 0.2)': '--accent-muted',
  'rgba(56, 189, 248, 0.16)': '--accent-muted',
  'rgba(56, 189, 248, 0.06)': '--accent-subtle',
  'rgba(125, 211, 252, 0.85)': '--accent-border-strong',
  'rgba(125, 211, 252, 0.95)': '--accent-border-strong',
  'rgba(125, 211, 252, 0.7)': '--accent-border',
  'rgba(125, 211, 252, 0.55)': '--accent-border',
  'rgba(248, 113, 113, 0.14)': '--danger-muted',
  'rgba(220, 38, 38, 0.9)': '--danger-strong',
  '#ff5f57': '--win-close',
  '#febc2e': '--win-minimize',
  '#28c840': '--win-maximize',
}

const SURFACE = {
  '#000': '--surface-base',
  '#000000': '--surface-base',
  'rgba(12, 12, 18, 0.95)': '--surface-raised',
  [B(0.5)]: '--surface-sunken',
  [B(0.6)]: '--surface-sunken',
  [B(0.55)]: '--surface-smoke',
  [B(0.85)]: '--fill-inverse-strong',
}

const STROKE = {
  [W(0.95)]: '--stroke-strong',
  [W(0.7)]: '--stroke-strong',
  [W(0.55)]: '--stroke-strong',
  [W(0.5)]: '--stroke-strong',
  [W(0.45)]: '--stroke-strong',
  [W(0.4)]: '--stroke-strong',
  [W(0.35)]: '--stroke-strong',
  [W(0.3)]: '--stroke-strong',
  [W(0.32)]: '--stroke-strong',
  [W(0.25)]: '--stroke-strong',
  [W(0.2)]: '--stroke-strong',
  [W(0.18)]: '--stroke-default',
  [W(0.16)]: '--stroke-default',
  [W(0.14)]: '--stroke-default',
  [W(0.12)]: '--stroke-default',
  [W(0.1)]: '--stroke-subtle',
  [W(0.08)]: '--stroke-subtle',
  [W(0.06)]: '--stroke-subtle',
  [W(0.05)]: '--stroke-subtle',
  'rgba(125, 211, 252, 0.6)': '--accent-border',
  'rgba(125, 211, 252, 0.95)': '--accent-border-strong',
  'rgba(125, 211, 252, 0.85)': '--accent-border-strong',
  'rgba(56, 189, 248, 0.7)': '--accent-border',
  '#7dd3fc': '--accent',
  '#000': '--stroke-inverse',
  [B(0.5)]: '--stroke-inverse',
  [B(0.2)]: '--stroke-inverse',
  [B(0.4)]: '--stroke-inverse',
  [B(0.3)]: '--stroke-inverse',
  [B(0.28)]: '--stroke-inverse',
  [B(0.06)]: '--stroke-inverse',
}

const tables = { color: TEXT, background: FILL, 'background-color': FILL, border: STROKE, 'border-color': STROKE, 'border-top': STROKE, 'border-bottom': STROKE, 'border-left': STROKE, 'border-right': STROKE, outline: STROKE, 'outline-color': STROKE }

// Literals that must survive verbatim (they are not part of the palette).
const KEEP = new Set(['transparent', 'none', 'inherit', 'currentColor'])

const files = []
const walk = (d) => {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name)
    if (e.isDirectory()) walk(p)
    else if (e.name.endsWith('.css')) files.push(p)
  }
}
walk('app/src')

const unmapped = new Map()
let changed = 0
const applied = new Map()

for (const file of files) {
  if (file.includes('index.css')) continue // the token file itself
  let css = fs.readFileSync(file, 'utf8')
  const original = css
  css = css.replace(/([\w-]+)\s*:\s*([^;{}]+)([;}])/g, (match, prop, value, tail) => {
    const table = tables[prop.trim().toLowerCase()]
    if (!table) return match
    const v = value.trim()
    if (v.includes('gradient') || v.includes('var(')) return match
    if (KEEP.has(v)) return match
    const token = table[v]
    if (token) {
      applied.set(token, (applied.get(token) || 0) + 1)
      changed += 1
      return `${prop.trim()}: var(${token})${tail}`
    }
    // A shorthand (`border: 1px solid rgba(...)`) — map just the colour part.
    const colour = v.match(/#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)/)
    if (colour && (prop.trim().toLowerCase().startsWith('border') || prop.trim().toLowerCase().startsWith('outline'))) {
      const c = colour[0]
      const cToken = STROKE[c]
      if (cToken) {
        applied.set(cToken, (applied.get(cToken) || 0) + 1)
        changed += 1
        return `${prop.trim()}: ${v.replace(c, `var(${cToken})`)}${tail}`
      }
    }
    unmapped.set(`${prop.trim()}: ${v}`, (unmapped.get(`${prop.trim()}: ${v}`) || 0) + 1)
    return match
  })
  if (css !== original) {
    if (!DRY) fs.writeFileSync(file, css)
    console.log(`rewrote ${file.replace(/\\/g, '/')}`)
  }
}

console.log(`\n${DRY ? '[dry] ' : ''}${changed} declarations mapped`)
console.log('\ntokens used:')
for (const [t, n] of [...applied.entries()].sort((a, b) => b[1] - a[1])) console.log(`   ${String(n).padStart(4)}x  var(${t})`)
if (unmapped.size) {
  console.log('\nUNMAPPED (need a hand):')
  for (const [k, n] of [...unmapped.entries()].sort((a, b) => b[1] - a[1])) console.log(`   ${String(n).padStart(4)}x  ${k}`)
}
