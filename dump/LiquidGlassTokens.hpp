#pragma once
#include <d2d1.h>

// ─────────────────────────────────────────────────────────────────────────────
// Liquid Glass Design Language — Design Tokens (v1.0)
//
// Every glass surface in the REX shell is described by a GlassLayer (blur +
// tint + noise).  Three canonical layers are defined:
//
//   Base     — sidebar, full‑height background panels  (60 px blur)
//   Surface  — cards, toolbars, status bars            (40 px blur)
//   Elevated — modals, context menus, dropdowns        (80 px blur)
//
// Edge highlights, shadows, and spring‑based motion tokens round out the set.
// All values are themeable and resolve through the TokenManager/StyleResolver.
// ─────────────────────────────────────────────────────────────────────────────

/// A single glass material layer.
struct GlassLayer {
    D2D1_COLOR_F tint;          ///< Premultiplied alpha tint colour.
    float        blurRadius;    ///< Standard deviation for Gaussian blur (px).
    float        noiseOpacity;  ///< [0, 1] opacity of the additive noise overlay.
};

/// Edge highlight along the top/left border of elevated surfaces.
struct GlassEdgeHighlight {
    D2D1_COLOR_F color;         ///< Bright semi‑transparent stroke.
    float        width;         ///< Stroke width (px).
};

/// Soft, coloured drop shadow for glass surfaces.
struct GlassShadow {
    D2D1_COLOR_F color;         ///< Shadow colour (picks up wallpaper hue).
    float        blur;          ///< Shadow blur radius (px).
    float        offsetY;       ///< Vertical offset (px).
};

/// Spring‑based motion and timing.
struct GlassMotion {
    float hoverLiftPx;          ///< translateZ lift on hover (px).
    float hoverStiffness;       ///< Spring stiffness for hover lift.
    float hoverDamping;         ///< Spring damping for hover lift.
    float pressScale;           ///< Scale factor on press (e.g. 0.98).
    float transitionMs;         ///< Default ease‑out transition duration (ms).
};

// ─────────────────────────────────────────────────────────────────────────────
// Complete token set for a single theme variant (light or dark).
// ─────────────────────────────────────────────────────────────────────────────
struct LiquidGlassTokens {
    GlassLayer          base;
    GlassLayer          surface;
    GlassLayer          elevated;
    GlassEdgeHighlight  edgeHighlight;
    GlassShadow         shadow;
    GlassMotion         motion;

    // ── Factory helpers ────────────────────────────────────────────────────

    /// Default dark theme tokens (spec §5, "Dark Theme (default)" column).
    static LiquidGlassTokens DefaultDark() {
        LiquidGlassTokens t{};

        // Base layer — sidebar / shell background
        t.base.tint         = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.60f);
        t.base.blurRadius   = 60.0f;
        t.base.noiseOpacity = 0.02f;

        // Surface layer — cards, toolbars
        t.surface.tint         = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f);
        t.surface.blurRadius   = 40.0f;
        t.surface.noiseOpacity = 0.015f;

        // Elevated layer — modals, dropdowns
        t.elevated.tint         = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f);
        t.elevated.blurRadius   = 80.0f;
        t.elevated.noiseOpacity = 0.025f;

        // Edge highlight
        t.edgeHighlight.color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
        t.edgeHighlight.width = 1.0f;

        // Shadow
        t.shadow.color   = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f);
        t.shadow.blur    = 24.0f;
        t.shadow.offsetY = 6.0f;

        // Motion
        t.motion.hoverLiftPx   = 4.0f;
        t.motion.hoverStiffness = 300.0f;
        t.motion.hoverDamping   = 25.0f;
        t.motion.pressScale     = 0.98f;
        t.motion.transitionMs   = 300.0f;

        return t;
    }

    /// Default light theme tokens (spec §5, "Light Theme" column).
    static LiquidGlassTokens DefaultLight() {
        LiquidGlassTokens t{};

        t.base.tint         = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.70f);
        t.base.blurRadius   = 60.0f;
        t.base.noiseOpacity = 0.02f;

        t.surface.tint         = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.80f);
        t.surface.blurRadius   = 40.0f;
        t.surface.noiseOpacity = 0.015f;

        t.elevated.tint         = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f);
        t.elevated.blurRadius   = 80.0f;
        t.elevated.noiseOpacity = 0.025f;

        t.edgeHighlight.color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f);
        t.edgeHighlight.width = 1.0f;

        t.shadow.color   = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f);
        t.shadow.blur    = 16.0f;
        t.shadow.offsetY = 4.0f;

        t.motion.hoverLiftPx   = 4.0f;
        t.motion.hoverStiffness = 300.0f;
        t.motion.hoverDamping   = 25.0f;
        t.motion.pressScale     = 0.98f;
        t.motion.transitionMs   = 300.0f;

        return t;
    }
};
