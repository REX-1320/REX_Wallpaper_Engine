/**
 * SolidMaterial.tsx
 *
 * The ONLY fully implemented material in Phase 1.
 *
 * It renders a plain, opaque surface using tokens exclusively — no
 * hard-coded visual values anywhere:
 *   - background  → colors.surface.base
 *   - radius      → radius.md
 *   - border      → border.width.hairline + colors.border.default
 *   - shadow      → shadow scale at the surface's elevation
 *
 * This file is the reference implementation every future material will
 * mirror: read the resolved tokens, render, stay pure.
 */

import type { CSSProperties } from 'react';

import type { MaterialProps } from '../core/SurfaceTypes';
import { shadowToCss } from '../tokens/shadow';

export function SolidMaterial({ children, className, style, tokens, elevation }: MaterialProps) {
  const surfaceStyle: CSSProperties = {
    backgroundColor: tokens.colors.surface.base,
    borderRadius: tokens.radius.md,
    border: `${tokens.border.width.hairline} solid ${tokens.colors.border.default}`,
    boxShadow: shadowToCss(tokens.shadow[elevation]),
    ...style,
  };

  return <div className={className} style={surfaceStyle}>{children}</div>;
}
