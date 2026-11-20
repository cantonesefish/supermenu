#pragma once

#include <cstdint>
#include <d2d1.h>

namespace sm {

// Must match cbuffer GlassParams in shaders/glass.hlsl byte for byte (400 bytes).
struct GlassParams {
    float sizeX = 0, sizeY = 0;
    float roiOriginX = 0, roiOriginY = 0;
    float roiSizeX = 1, roiSizeY = 1;
    float radius = 0, edge = 12;
    float refract = 12, dispersion = 0.3f;
    float blurLod = 3.0f, tintMix = 0.06f;
    float tintR = 1, tintG = 1, tintB = 1, tintA = 1;
    float highlight = 0.8f, shadow = 0.35f;
    float lightX = -0.2f, lightY = -0.98f;
    float inset = 10, blurMix = 1, fade = 0;
    float noise = 0.015f;  // Pad0 in the shader
    float shapeL = 0, shapeT = 0, shapeR = 1, shapeB = 1;
    float rippleX = 0, rippleY = 0, rippleR = 0, rippleA = 0;
    float parts[6][4] = {};
    float partRadii[6][4] = {};
    float partCount = 0, melt = 0, partMix = 0;
    float vibrancy = 1.15f;  // Pad1 in the shader
    float tailX = 0, tailHalfWidth = 0, tailHeight = 0, tailInset = 0;
    float waveX = 0, waveWidth = 0, waveHeight = 0, wavePad = 0;
    float fromArea[6] = {1, 0, 0, 1, 0, 0};
    float fromAreaPad[2] = {0, 0};
};
static_assert(sizeof(GlassParams) == 400, "GlassParams must match glass.hlsl cbuffer");

struct RectF {
    float left = 0, top = 0, right = 0, bottom = 0;
};

}  // namespace sm
