#pragma once
#include <d2d1.h>

struct GlowStyle {
    D2D1::ColorF textColor      = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    D2D1::ColorF glowColor      = D2D1::ColorF(0.6f, 0.8f, 1.0f, 1.0f);
    D2D1::ColorF placeholderColor = D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f);
    float glowIntensity = 0.8f;
};
