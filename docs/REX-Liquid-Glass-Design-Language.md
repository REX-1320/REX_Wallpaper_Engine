# REX Liquid Glass Design Language

**Version**: 1.0  
**Based on**: modern glassmorphism principles, DWM composition, and Direct2D  
**Status**: Design Specification – implementation ready

---

## 1. Philosophy & Overview

Liquid Glass is an evolution of glassmorphism that emphasises **depth, refraction, and fluidity**. It transforms the REX shell into an ethereal, lightweight layer that feels physically present without obstructing the desktop beneath it. Every surface behaves like a thin sheet of frosted glass with subtle, dynamic lighting that responds to the content behind it and the user's interactions.

### Core Principles
- **Depth without weight** – panels float above the desktop with soft shadows and parallax‑like blur.
- **Light reactivity** – glass surfaces subtly shift colour and brightness based on the wallpaper behind them.
- **Fluid motion** – every transition, hover, and press uses spring‑based physics to feel liquid and responsive.
- **Optical precision** – blur, noise, tint, and edge highlights are tuned to create a realistic glass material, not a flat overlay.
- **Inclusive & accessible** – glass effects degrade gracefully on low‑end hardware and respect high‑contrast / reduced‑motion settings.

---

## 2. Visual Characteristics

### 2.1 Refractive Blur
The most distinctive feature: instead of a simple Gaussian blur, Liquid Glass applies a **bilateral or depth‑aware blur** that preserves edges of underlying content (like icons or text) while softening low‑contrast areas. On Windows, we approximate this with a combination of:
- **Acrylic blur** (DWM) for the base layer
- **Directional blur** at surface edges to simulate refraction
- **Noise texture** (2% opacity) to add tactility

### 2.2 Dynamic Tint
The glass colour shifts based on the average colour of the wallpaper behind it. A cool, desaturated tint is applied during the day; a warmer tint at night. The tint is expressed as a subtle gradient across the surface.

### 2.3 Edge Highlighting
A 1‑px inner border with a bright, semi‑transparent stroke (opacity 0.15–0.25) runs along the top and left edges of elevated surfaces, simulating light catching the edge of the glass.

### 2.4 Shadow & Depth
Surfaces at different elevations cast soft, coloured drop shadows (not black). The shadow colour picks up the wallpaper's dominant hue at low opacity, grounding the glass in the scene.

### 2.5 Motion & Spring Physics
All interactive elements use critically damped springs. When a card is hovered, it lifts slightly (translate Z) with a corresponding increase in blur radius and shadow spread, making the glass feel physically responsive.

---

## 3. Layering Model

```text
┌────────────────────────────────────────┐
│ 1. Desktop Wallpaper                   │  ← visible through all layers
├────────────────────────────────────────┤
│ 2. Background Acrylic (Shell)          │  ← 80% opacity, 60px blur, noise
├────────────────────────────────────────┤
│ 3. Workspace / Canvas                  │  ← transparent, receives content
├────────────────────────────────────────┤
│ 4. Surface Cards                       │  ← 95% opacity, 40px blur, tinted
├────────────────────────────────────────┤
│ 5. Elevated Panels (Dialogs, Dropdowns)│  ← 98% opacity, 80px blur, shadow
├────────────────────────────────────────┤
│ 6. Content (Text, Icons, Controls)     │  ← opaque, crisp
└────────────────────────────────────────┘
```

Each layer uses a distinct glass recipe defined by tokens.

---

## 4. Implementation on Windows

REX targets Windows 10 (1903+) and Windows 11, utilising native composition capabilities.

### 4.1 Shell Window
- `WS_EX_LAYERED` with per‑pixel alpha, or better: `WS_EX_NOREDIRECTIONBITMAP` and DirectComposition.
- Background acrylic: set via `DwmEnableBlurBehindWindow` or the `WCA_ACCENT_POLICY` (AccentState = `ACCENT_ENABLE_ACRYLICBLURBEHIND`).
- Fallback: manually composite a blurred desktop screenshot with a noise texture for older systems.

### 4.2 Surfaces & Cards
- Render as Direct2D layers using `ID2D1DeviceContext` and `ID2D1Effect` (Gaussian blur, colour matrix, noise).
- The `LiquidGlassBrush` encapsulates blur radius, tint colour, noise intensity, and edge highlight.
- Performance: blur radii < 40px are computed in real time; above 40px, a pre‑blurred background bitmap is sampled.

### 4.3 Tinting Strategy
- Sample the desktop wallpaper colour using `IWICImagingFactory` to decode a thumbnail, then compute average RGB in a grid of 8×8 cells.
- Blend the wallpaper average with a fixed cool/warm tint based on time of day (via `Settings` – light/dark mode).

### 4.4 Accessibility & Fallbacks
- **High Contrast**: replace glass with a solid dark/light surface.
- **Reduced Motion**: disable blur animations, use static blur.
- **Performance Mode**: reduce blur radii, lower noise resolution, use simpler shadows.

---

## 5. Design Tokens (Liquid Glass)

All values are themeable and live under the `Glass` token category.

| Token                           | Light Theme                 | Dark Theme (default)          |
|---------------------------------|-----------------------------|-------------------------------|
| `Glass.Base.Tint`               | rgba(255,255,255,0.70)      | rgba(0,0,0,0.60)              |
| `Glass.Base.BlurRadius`         | 60px                        | 60px                          |
| `Glass.Base.NoiseOpacity`       | 0.02                        | 0.02                          |
| `Glass.Base.NoiseScale`         | 1.0                         | 1.0                           |
| `Glass.Surface.Tint`            | rgba(255,255,255,0.80)      | rgba(0,0,0,0.75)              |
| `Glass.Surface.BlurRadius`      | 40px                        | 40px                          |
| `Glass.Surface.NoiseOpacity`    | 0.015                       | 0.015                         |
| `Glass.Elevated.Tint`           | rgba(255,255,255,0.90)      | rgba(0,0,0,0.85)              |
| `Glass.Elevated.BlurRadius`     | 80px                        | 80px                          |
| `Glass.Elevated.NoiseOpacity`   | 0.025                       | 0.025                         |
| `Glass.EdgeHighlight.Color`     | rgba(255,255,255,0.20)      | rgba(255,255,255,0.15)        |
| `Glass.EdgeHighlight.Width`     | 1px                         | 1px                           |
| `Glass.Shadow.Color`            | rgba(0,0,0,0.12)            | rgba(0,0,0,0.25)              |
| `Glass.Shadow.Blur`             | 16px                        | 24px                          |
| `Glass.Shadow.OffsetY`          | 4px                         | 6px                           |

### Motion Tokens
- `Glass.Motion.HoverLift`: 4px (translateZ)
- `Glass.Motion.HoverDuration`: 250ms (spring)
- `Glass.Motion.PressScale`: 0.98
- `Glass.Motion.Transition`: 300ms ease‑out

---

## 6. Component Styling with Liquid Glass

### 6.1 Glass Card
- **Background**: `Glass.Surface` tokens.
- **Border**: 1px `Glass.EdgeHighlight` (top/left only), 1px `Border.Default` (bottom/right).
- **Shadow**: `Glass.Shadow` on hover.
- **Corner radius**: 16px.

### 6.2 Sidebar / Navigation
- **Background**: `Glass.Base` tokens, extended full height.
- **Active item**: subtle background accent with 15% opacity, no blur change.

### 6.3 Command Palette / Modal
- **Backdrop**: dark overlay with 50% opacity and 8px blur (simulates depth).
- **Dialog panel**: `Glass.Elevated` tokens, scale entrance (0.95→1.0 spring).

### 6.4 Toolbar / Status Bar
- **Background**: `Glass.Surface` with reduced blur (20px) to keep them crisp.
- **Separators**: 1px line with `Border.Default`.

### 6.5 Buttons (Primary, Ghost)
- **Primary**: solid accent background with subtle inset shadow; on hover, a soft glow (box‑shadow with accent colour at 30% opacity).
- **Ghost**: transparent background; on hover, a 10% accent tint with 20px blur.

---

## 7. Animation & Interaction

### 7.1 Hover / Focus
- Cards lift 4px, shadow grows, blur radius increases by 20%.
- Smooth spring: stiffness 300, damping 25.

### 7.2 Page Transitions
- New page fades in (opacity 0→1) and slides 8px from the right, all within 250ms.

### 7.3 Sidebar Expand/Collapse
- Width animates with a spring (stiffness 200, damping 30), icons fade to labels.

### 7.4 Notification Toast
- Slides in from bottom‑right with a gentle bounce (overshoot spring).

### 7.5 Background Parallax
- As the mouse moves, the glass tint and blur origin shift slightly (2–4px) to simulate light refraction. This is subtle and can be disabled.

---

## 8. Code Implementation Example (Direct2D)

```cpp
// Creating a Liquid Glass brush for a card surface
void CreateLiquidGlassCardBrush(ID2D1DeviceContext* dc, const GlassTokens& tokens) {
    // 1. Gaussian blur effect on the background bitmap
    ComPtr<ID2D1Effect> blurEffect;
    dc->CreateEffect(CLSID_D2D1GaussianBlur, &blurEffect);
    blurEffect->SetInput(0, backgroundBitmap);
    blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                         tokens.surfaceBlurRadius / 2.0f);

    // 2. Tint with colour matrix
    ComPtr<ID2D1Effect> tintEffect;
    dc->CreateEffect(CLSID_D2D1ColorMatrix, &tintEffect);
    D2D1_MATRIX_5X4_F tintMatrix = BuildTintMatrix(tokens.tintColor);
    tintEffect->SetInput(0, blurEffect.Get());
    tintEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, tintMatrix);

    // 3. Add noise
    ComPtr<ID2D1Effect> noiseEffect;
    dc->CreateEffect(CLSID_D2D1DisplacementMap, &noiseEffect); // or turbulence
    // ... composite with BlendEffect ...

    // 4. Edge highlight: draw a stroked rounded rectangle after filling
}
```

---

## 9. Fallback Strategy

- **Windows 10 pre‑1903**: Use `UpdateLayeredWindow` with pre‑blurred screenshots; no dynamic blur.
- **Low‑end GPUs**: Disable noise, reduce blur radius to 20px, use solid shadows.
- **Battery saver**: Switch to static acrylic (Mica) via `DwmSetWindowAttribute`.
- **High contrast themes**: Replace glass with a solid colour from the high‑contrast palette.

---

## 10. References & Inspiration

- Apple Human Interface Guidelines – Materials
- Microsoft Fluent Design System – Acrylic, Mica
- DWM composition and DirectComposition documentation
- Research: Real‑time bilateral blur approximations for UI

---

*This Liquid Glass design language extends the REX Design System and should be used for all shell and module UI surfaces. It is optimised for Direct2D + DirectComposition and gracefully degrades on unsupported systems.*
