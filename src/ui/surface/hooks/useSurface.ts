/**
 * useSurface.ts
 *
 * The one hook components use to read the surface system:
 *
 *     const surface = useSurface();
 *     const radius = surface.radius.md;
 *
 * It never throws: outside a provider the context falls back to the
 * framework defaults (solid / dark / balanced) defined in
 * SurfaceContext.tsx, so components can be developed and rendered in
 * isolation without a full app tree.
 */

import { useContext } from 'react';
import { SurfaceContext, type SurfaceContextValue } from '../core/SurfaceContext';

export function useSurface(): SurfaceContextValue {
  return useContext(SurfaceContext);
}
