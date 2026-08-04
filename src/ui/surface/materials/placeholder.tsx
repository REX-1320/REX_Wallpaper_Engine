/**
 * placeholder.tsx
 *
 * Internal helper for the Phase 1 placeholder materials.
 *
 * Phase 1 is ARCHITECTURE ONLY: Glass, Acrylic, Mica, LiquidGlass and
 * Transparent must exist and compile, but must not contain any effect.
 *
 * Architectural decision — graceful degradation over crashes: instead of
 * throwing "Not implemented", every placeholder renders the Solid material
 * (the one renderable material). This keeps the app tree functional while
 * the materials are being built, and gives every placeholder a single,
 * consistent fallback behavior. When a material is implemented, its file
 * stops using this helper — nothing else changes, because the Surface
 * component already resolves renderers by material name.
 *
 * The displayName is set from the material name so React DevTools shows
 * e.g. <GlassMaterial> even while it renders Solid underneath.
 */

import type { SurfaceRenderer } from '../core/SurfaceTypes';
import type { MaterialProps } from '../core/SurfaceTypes';
import { SolidMaterial } from './SolidMaterial';

export function createPlaceholderMaterial(name: string): SurfaceRenderer {
  function PlaceholderMaterial(props: MaterialProps) {
    return <SolidMaterial {...props} />;
  }
  PlaceholderMaterial.displayName = name;
  return PlaceholderMaterial;
}
