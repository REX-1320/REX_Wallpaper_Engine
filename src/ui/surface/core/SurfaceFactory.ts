/**
 * SurfaceFactory.ts
 *
 * Maps a material type → its renderer component.
 *
 * Architectural decision: resolution is REQUEST-BASED, not import-based.
 * `Surface` never imports materials; it calls `createSurfaceRenderer`
 * and gets back whichever renderer is currently registered for that
 * material. Because custom surfaces register with the registry, swapping
 * a material's renderer (or adding a brand-new one) requires zero changes
 * to the factory, the Surface component, or any consumer component.
 *
 * Resolution order:
 *   1. Registry lookup (custom registrations / overrides win).
 *   2. Built-in fallback map (guarantees the six core materials always
 *      resolve, even if someone unregisters a built-in).
 *   3. Throw — an unknown material is a programming error, surface it.
 *
 * Built-ins are eagerly self-registered at module load (idempotent) so
 * the registry is populated even before the first React render.
 */

import type { MaterialType, SurfaceRenderer } from './SurfaceTypes';
import { MATERIALS } from './SurfaceTypes';
import { surfaceRegistry } from './SurfaceRegistry';

import { SolidMaterial } from '../materials/SolidMaterial';
import { GlassMaterial } from '../materials/GlassMaterial';
import { AcrylicMaterial } from '../materials/AcrylicMaterial';
import { MicaMaterial } from '../materials/MicaMaterial';
import { LiquidGlassMaterial } from '../materials/LiquidGlassMaterial';
import { TransparentMaterial } from '../materials/TransparentMaterial';

/** Static fallback map — the six core materials can never be missing. */
const BUILT_IN_RENDERERS: Record<MaterialType, SurfaceRenderer> = {
  solid: SolidMaterial,
  glass: GlassMaterial,
  acrylic: AcrylicMaterial,
  mica: MicaMaterial,
  liquidGlass: LiquidGlassMaterial,
  transparent: TransparentMaterial,
};

/**
 * Registers the six built-in materials into the shared registry.
 * Idempotent: safe under hot reload / repeated module evaluation.
 */
export function registerBuiltInSurfaces(): void {
  for (const material of MATERIALS) {
    if (surfaceRegistry.getSurface(material) === undefined) {
      surfaceRegistry.registerSurface({
        id: material,
        material,
        theme: 'dark',
        state: 'active',
        renderer: BUILT_IN_RENDERERS[material],
      });
    }
  }
}

// Eager, idempotent self-registration (see module docs above).
registerBuiltInSurfaces();

/**
 * Resolves the renderer for a material.
 * Custom-registered renderers override built-ins.
 */
export function createSurfaceRenderer(material: MaterialType): SurfaceRenderer {
  const registered = surfaceRegistry.getSurface(material);
  if (registered?.renderer !== undefined) {
    return registered.renderer;
  }
  const builtIn = BUILT_IN_RENDERERS[material];
  if (builtIn === undefined) {
    throw new Error(`SurfaceFactory: no renderer for material "${material}".`);
  }
  return builtIn;
}
