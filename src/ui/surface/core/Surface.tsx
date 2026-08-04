/**
 * Surface.tsx
 *
 * The universal surface container — the ONLY component every REX UI
 * component (Button, Card, Dialog, Toolbar, ...) will render through.
 *
 * Responsibilities (Phase 1 spec):
 *   1. Read context            → useSurface()
 *   2. Resolve material        → explicit `material` prop > `variant`
 *                                preset > provider default
 *   3. Request renderer        → createSurfaceRenderer(material)
 *   4. Render children         → through the resolved renderer
 *
 * Resolution rules:
 *   - `material` prop wins over everything.
 *   - Otherwise `variant` picks its preset from VARIANT_DEFAULTS
 *     (material + elevation). Variants are the component-facing API:
 *     `<Surface variant="card">` today renders Solid at elevation 1;
 *     when Liquid Glass ships, retargeting the preset restyles the app
 *     with NO component changes.
 *   - With neither prop, the provider's global material/elevation apply.
 *
 * This component holds NO styling logic — everything visual lives in the
 * material renderer, which receives the already-resolved tokens.
 */

import type { CSSProperties, ReactNode } from 'react';

import type { Elevation, MaterialType, SurfaceVariant } from './SurfaceTypes';
import { VARIANT_DEFAULTS } from './SurfaceTypes';
import { useSurface } from '../hooks/useSurface';
import { createSurfaceRenderer } from './SurfaceFactory';

export interface SurfaceProps {
  /** Explicit material override. Wins over `variant` and the provider. */
  material?: MaterialType;
  /** Semantic intent (card/overlay/elevated/...). Resolved via presets. */
  variant?: SurfaceVariant;
  /** Explicit elevation. Defaults to the variant preset / provider value. */
  elevation?: Elevation;
  className?: string;
  style?: CSSProperties;
  children?: ReactNode;
}

export function Surface({
  material,
  variant,
  elevation,
  className,
  style,
  children,
}: SurfaceProps): ReactNode {
  const surface = useSurface();

  // 2. Resolve material + elevation:
  //    explicit prop > variant preset > provider default.
  const resolvedMaterial: MaterialType =
    material ?? (variant !== undefined ? VARIANT_DEFAULTS[variant].material : surface.material);

  const resolvedElevation: Elevation =
    elevation ?? (variant !== undefined ? VARIANT_DEFAULTS[variant].elevation : surface.elevation);

  // 3. Request the renderer for the resolved material.
  //    Custom materials registered in the registry resolve transparently.
  const Renderer = createSurfaceRenderer(resolvedMaterial);

  // 4. Render children through the material, handing it the resolved tokens.
  return (
    <Renderer tokens={surface.tokens} elevation={resolvedElevation} className={className} style={style}>
      {children}
    </Renderer>
  );
}
